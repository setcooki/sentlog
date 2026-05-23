#!/usr/bin/env python3
"""Convert AASIST-L PyTorch checkpoint to ONNX for ORT inference.

Run once (or via CI release pipeline). Output: aasist_l.onnx that the
SDK's platform bindings load at runtime.

Provenance:
- Upstream: https://github.com/clovaai/aasist (MIT, pinned commit).
- Pretrained weights: models/weights/AASIST-L.pth in the upstream repo.
- Model architecture: models/AASIST.py in the upstream repo.

Output is committed to:
- android/sentlog/src/main/assets/aasist_l.onnx
- ios/Sources/Sentlog/Resources/aasist_l.onnx
- models/liveness/aasist_l.onnx (source of truth; both platforms copy from here)

Usage:
    python3 -m venv .venv
    source .venv/bin/activate
    pip install -r requirements.txt
    python convert-aasist-l.py

The script will:
1. Clone clovaai/aasist at the pinned commit into a temp dir.
2. Load AASIST-L.pth into the architecture defined in models/AASIST.py.
3. Trace + export to ONNX with a dynamic-length audio input.
4. Verify the ONNX file by running a single dummy inference via ORT.
5. Write aasist_l.onnx to models/liveness/.
6. Copy to both platform asset directories.

The conversion is one-shot — re-run if the upstream checkpoint or our
target ORT opset version changes.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# --- Pinned upstream ---

AASIST_REPO  = "https://github.com/clovaai/aasist"
# Commit SHA of clovaai/aasist main at the time this script was written.
# Update to a newer SHA only after auditing for breaking changes to
# the model architecture or weight format.
AASIST_COMMIT = "a04c9863f63d44471dde8a6f5a3a14f86b4a76e2"

# Target ONNX opset. 17 is well-supported by ORT 1.17.x and includes
# all ops AASIST-L uses (RawNet-style convolutions + GAT layers).
ONNX_OPSET = 17

# AASIST-L expects 4 s of audio at 16 kHz = 64 000 samples as the
# typical inference window. The ONNX export is done with a dynamic
# length axis so the runtime can pad/truncate to whatever it has.
SAMPLE_RATE_HZ = 16_000
TYPICAL_SAMPLES = SAMPLE_RATE_HZ * 4


def run(cmd, **kwargs):
    print(f">>> {' '.join(cmd)}", file=sys.stderr)
    return subprocess.run(cmd, check=True, **kwargs)


def clone_aasist(workdir: Path) -> Path:
    aasist_dir = workdir / "aasist"
    run(["git", "clone", "--no-tags", AASIST_REPO, str(aasist_dir)])
    run(["git", "-C", str(aasist_dir), "checkout", AASIST_COMMIT])
    return aasist_dir


def export_to_onnx(aasist_dir: Path, out_path: Path) -> None:
    # Import lazily so the script's --help works without torch installed.
    sys.path.insert(0, str(aasist_dir))
    import json
    import torch
    from models.AASIST import Model  # type: ignore

    # AASIST-L config is in the upstream config_AASIST-L.conf JSON.
    config_path = aasist_dir / "config" / "AASIST-L.conf"
    with open(config_path) as f:
        config = json.load(f)
    model_config = config["model_config"]

    model = Model(model_config)
    weights_path = aasist_dir / "models" / "weights" / "AASIST-L.pth"
    state_dict = torch.load(weights_path, map_location="cpu")
    # Upstream checkpoints sometimes wrap weights — handle both shapes.
    if "model_state_dict" in state_dict:
        state_dict = state_dict["model_state_dict"]
    model.load_state_dict(state_dict)
    model.eval()

    # Trace with the typical input size. The exported graph uses a
    # dynamic axis so runtime can feed any length.
    dummy = torch.zeros(1, TYPICAL_SAMPLES, dtype=torch.float32)

    torch.onnx.export(
        model,
        dummy,
        str(out_path),
        opset_version=ONNX_OPSET,
        input_names=["audio"],
        output_names=["embedding", "logits"],
        dynamic_axes={
            "audio":     {0: "batch", 1: "samples"},
            "embedding": {0: "batch"},
            "logits":    {0: "batch"},
        },
    )
    print(f">>> wrote {out_path} ({out_path.stat().st_size} bytes)", file=sys.stderr)


def verify_onnx(out_path: Path) -> None:
    """Smoke-test the exported model via ORT."""
    import numpy as np
    import onnxruntime as ort

    sess = ort.InferenceSession(str(out_path),
                                providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name
    dummy = np.zeros((1, TYPICAL_SAMPLES), dtype=np.float32)
    outputs = sess.run(None, {input_name: dummy})
    print(f">>> ORT smoke test passed; outputs: "
          f"{[o.shape for o in outputs]}", file=sys.stderr)


def copy_to_platforms(out_path: Path, repo_root: Path) -> None:
    android_dst = repo_root / "android" / "sentlog" / "src" / "main" / "assets" / "aasist_l.onnx"
    ios_dst     = repo_root / "ios"     / "Sources" / "Sentlog" / "Resources" / "aasist_l.onnx"
    android_dst.parent.mkdir(parents=True, exist_ok=True)
    ios_dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(out_path, android_dst)
    shutil.copy2(out_path, ios_dst)
    print(f">>> copied to {android_dst} and {ios_dst}", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--skip-verify",   action="store_true",
                    help="Skip the ORT smoke test (faster, no onnxruntime needed)")
    ap.add_argument("--skip-platforms", action="store_true",
                    help="Don't copy the .onnx into the AAR / SwiftPM resource dirs")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent.parent
    models_dir = repo_root / "models" / "liveness"
    models_dir.mkdir(parents=True, exist_ok=True)
    out_path = models_dir / "aasist_l.onnx"

    with tempfile.TemporaryDirectory(prefix="aasist-convert-") as tmp:
        tmp_path = Path(tmp)
        aasist_dir = clone_aasist(tmp_path)
        export_to_onnx(aasist_dir, out_path)

    if not args.skip_verify:
        verify_onnx(out_path)
    if not args.skip_platforms:
        copy_to_platforms(out_path, repo_root)

    print(f">>> done. Pinned commit: {AASIST_COMMIT}", file=sys.stderr)
    print(f">>> Record the commit SHA in models/liveness/SOURCE.md.", file=sys.stderr)


if __name__ == "__main__":
    main()
