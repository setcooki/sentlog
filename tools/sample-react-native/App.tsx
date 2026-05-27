// Sentlog — minimal React Native sample. Drop into a fresh
// `npx react-native init` app's App.tsx. See ./README.md for
// install + permission setup.

import React, { useEffect, useState } from "react";
import {
    Button, SafeAreaView, ScrollView, StyleSheet, Text, View,
} from "react-native";

import {
    Sentlog, onTelemetry,
    type SentlogConfig, type SentlogResult, type TelemetryEvent,
} from "@sentlog/react-native";

const DEFAULT_CONFIG: SentlogConfig = {
    issuer: "did:example:integrator",
    language_hint: "en",
    target_phrase: "please confirm your name",
    min_duration_ms: 2000,
};

export default function App() {
    const [status, setStatus] = useState<string>("idle");
    const [result, setResult] = useState<SentlogResult | null>(null);
    const [error,  setError]  = useState<string | null>(null);

    // Surface telemetry in the UI for debugging. Not how integrators
    // typically wire it in production — usually goes to a logger.
    const [events, setEvents] = useState<TelemetryEvent[]>([]);

    useEffect(() => {
        let mounted = true;
        const sub = onTelemetry((ev) => {
            if (!mounted) return;
            setEvents((prev) => [...prev.slice(-10), ev]);
        });
        return () => { mounted = false; sub.remove(); };
    }, []);

    async function start() {
        setError(null);
        setResult(null);
        try {
            await Sentlog.initialize(DEFAULT_CONFIG);
            setStatus("starting");
            await Sentlog.startCapture();
            setStatus("capturing");
        } catch (e: unknown) {
            // Common case: microphone permission denied. The integrator
            // is responsible for prompting; here we just surface the
            // error message.
            setError(String(e));
            setStatus("idle");
        }
    }

    async function stop() {
        setStatus("evaluating");
        try {
            const r = await Sentlog.evaluate();
            setResult(r);
            setStatus("done");
        } catch (e: unknown) {
            setError(String(e));
            setStatus("idle");
        }
    }

    async function reset() {
        try { await Sentlog.destroy(); } catch { /* ignore */ }
        setResult(null);
        setError(null);
        setEvents([]);
        setStatus("idle");
    }

    return (
        <SafeAreaView style={styles.container}>
            <ScrollView contentContainerStyle={styles.body}>
                <Text style={styles.title}>Sentlog sample</Text>
                <Text style={styles.status}>state: {status}</Text>

                <View style={styles.row}>
                    <Button title="start" onPress={start}
                            disabled={status !== "idle" && status !== "done"} />
                    <Button title="stop+eval" onPress={stop}
                            disabled={status !== "capturing"} />
                    <Button title="reset" onPress={reset} />
                </View>

                {error && (
                    <Text style={styles.error}>error: {error}</Text>
                )}

                {result && (
                    <View style={styles.section}>
                        <Text style={styles.h2}>Result</Text>
                        <Text>transcript: {result.transcript.text}</Text>
                        <Text>language:   {result.transcript.language}</Text>
                        <Text>target_match: {String(result.transcript.target_match)}</Text>
                        <Text>direction:  {result.direction.toFixed(3)}</Text>
                        <Text>liveness:   {result.liveness.score.toFixed(3)}</Text>
                        <Text>engagement: {result.engagement.score.toFixed(3)}
                              {" "}({result.engagement.model_status})</Text>
                        <Text>presence:   {result.presence.summary.toFixed(3)}</Text>
                        <Text>grade:      {result.evidence_grade}</Text>
                    </View>
                )}

                {events.length > 0 && (
                    <View style={styles.section}>
                        <Text style={styles.h2}>Telemetry (last 10)</Text>
                        {events.map((ev, i) => (
                            <Text key={i} style={styles.mono}>
                                {JSON.stringify(ev)}
                            </Text>
                        ))}
                    </View>
                )}
            </ScrollView>
        </SafeAreaView>
    );
}

const styles = StyleSheet.create({
    container: { flex: 1, backgroundColor: "#fafafa" },
    body:      { padding: 16, gap: 12 },
    title:     { fontSize: 22, fontWeight: "600" },
    status:    { fontSize: 14, color: "#666" },
    row:       { flexDirection: "row", gap: 12 },
    error:     { color: "#a00", marginTop: 12 },
    section:   { marginTop: 24, gap: 4 },
    h2:        { fontSize: 16, fontWeight: "600", marginBottom: 6 },
    mono:      { fontFamily: "Menlo", fontSize: 11, color: "#444" },
});
