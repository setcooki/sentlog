import Foundation

/// HTTP fetch for untrusted endpoints: redirects are refused, so an https
/// URL can't hand off to somewhere else, and the download is cancelled as
/// soon as it passes `maxBytes` rather than checked once it has finished.
enum BoundedFetch {

    enum Result {
        case success(Data)
        case httpStatus(Int)   // not 2xx, including a refused redirect
        case oversize
        case failed(Error)
    }

    static func run(
        _ request: URLRequest,
        maxBytes: Int,
        configuration: URLSessionConfiguration = .ephemeral,
        completion: @escaping (Result) -> Void
    ) {
        let delegate = Delegate(maxBytes: maxBytes, completion: completion)
        let session = URLSession(configuration: configuration,
                                 delegate: delegate, delegateQueue: nil)
        let task = session.dataTask(with: request)
        delegate.task = task
        task.resume()
        session.finishTasksAndInvalidate()
    }

    private final class Delegate: NSObject, URLSessionDataDelegate {
        private let maxBytes: Int
        private let completion: (Result) -> Void
        private var received = Data()
        private var oversize = false
        var task: URLSessionTask?

        init(maxBytes: Int, completion: @escaping (Result) -> Void) {
            self.maxBytes = maxBytes
            self.completion = completion
        }

        func urlSession(_ session: URLSession, task: URLSessionTask,
                        willPerformHTTPRedirection response: HTTPURLResponse,
                        newRequest request: URLRequest,
                        completionHandler: @escaping (URLRequest?) -> Void) {
            completionHandler(nil)
        }

        func urlSession(_ session: URLSession, dataTask: URLSessionDataTask,
                        didReceive data: Data) {
            if received.count + data.count > maxBytes {
                oversize = true
                dataTask.cancel()
                return
            }
            received.append(data)
        }

        func urlSession(_ session: URLSession, task: URLSessionTask,
                        didCompleteWithError error: Error?) {
            if oversize {
                completion(.oversize)
            } else if let error {
                completion(.failed(error))
            } else {
                let code = (task.response as? HTTPURLResponse)?.statusCode ?? -1
                completion((200..<300).contains(code) ? .success(received) : .httpStatus(code))
            }
        }
    }
}
