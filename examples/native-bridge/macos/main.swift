import Foundation
import CoreFoundation

// stdout is reserved for the bridge protocol.
func send(_ message: [String: Any]) {
    guard let data = try? JSONSerialization.data(withJSONObject: message) else { exit(1) }
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data([10]))
}

var ready = false
while let line = readLine() {
    guard line.utf8.count <= 256 * 1024,
          let data = line.data(using: .utf8),
          let request = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
          let number = request["id"] as? NSNumber,
          CFGetTypeID(number) != CFBooleanGetTypeID(),
          number.doubleValue >= 1, number.doubleValue <= 9007199254740991,
          number.doubleValue.rounded(.towardZero) == number.doubleValue,
          let method = request["method"] as? String else { exit(1) }
    let id = number.int64Value
    guard method == "system.info" else {
        send(["id": id, "error": "Unsupported method"])
        continue
    }
    if let params = request["params"], !(params is NSNull),
       !((params as? [String: Any])?.isEmpty ?? false) {
        send(["id": id, "error": "system.info accepts only empty parameters"])
        continue
    }
    if !ready {
        send(["event": "system.ready", "data": ["protocol": 1]])
        ready = true
    }
    let process = ProcessInfo.processInfo
    send(["id": id, "result": [
        "platform": "macos", "osVersion": process.operatingSystemVersionString,
        "logicalProcessors": process.processorCount, "uptimeSeconds": process.systemUptime
    ]])
}
// EOF closes the helper; no daemon or detached child is created.
