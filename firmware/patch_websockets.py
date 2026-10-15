# The links2004 client drops the server close code and never exposes the
# HTTP status line. Patch those two spots before the library is compiled.
# Idempotent: a second build sees the markers and leaves the files alone.

Import("env")

import sys


STATUS_DECL = (
    'extern "C" void kommOnStatus(const char* line);\n'
    'extern "C" void kommOnArm(void);\n'
)
CLOSE_DECL = 'extern "C" void kommOnClose(unsigned code, const char* reason, size_t len);\n'
ARM_OLD = "void WebSocketsClient::sendHeader(WSclient_t * client) {\n    static const char * NEW_LINE = \"\\r\\n\";\n"
ARM_NEW = (
    "void WebSocketsClient::sendHeader(WSclient_t * client) {\n"
    "    kommOnArm();\n"
    "    static const char * NEW_LINE = \"\\r\\n\";\n"
)

STATUS_OLD = (
    '            client->cCode = headerLine->substring(9, headerLine->indexOf(\' \', 9)).toInt();\n'
)
STATUS_NEW = STATUS_OLD + "            kommOnStatus(headerLine->c_str());\n"

CLOSE_OLD = """            case WSop_close: {
#ifndef NODEBUG_WEBSOCKETS
                uint16_t reasonCode = 1000;
                if(header->payloadLen >= 2) {
                    reasonCode = payload[0] << 8 | payload[1];
                }
#endif
                DEBUG_WEBSOCKETS("[WS][%d][handleWebsocket] get ask for close. Code: %d\\n", client->num, reasonCode);
                if(header->payloadLen > 2) {
                    DEBUG_WEBSOCKETS(" (%s)\\n", (payload + 2));
                } else {
                    DEBUG_WEBSOCKETS("\\n");
                }
                clientDisconnect(client, 1000);
            } break;
"""

CLOSE_NEW = """            case WSop_close: {
                uint16_t reasonCode = 1000;
                const char* reasonText = "";
                size_t reasonLen = 0;
                if(header->payloadLen >= 2 && payload) {
                    reasonCode = ((uint16_t)payload[0] << 8) | payload[1];
                }
                if(header->payloadLen > 2 && payload) {
                    reasonText = (const char*)(payload + 2);
                    reasonLen = header->payloadLen - 2;
                }
                kommOnClose(reasonCode, reasonText, reasonLen);
                clientDisconnect(client, 1000);
            } break;
"""


def _patch(path, marker, old, new, decl):
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    if marker in text:
        return
    if old not in text:
        sys.stderr.write("WebSockets-patchen hittade inte texten i %s\n" % path)
        env.Exit(1)
    if decl not in text:
        anchor = '#include "WebSockets.h"\n'
        if anchor not in text:
            sys.stderr.write("WebSockets-patchen hittade inte include i %s\n" % path)
            env.Exit(1)
        text = text.replace(anchor, anchor + decl, 1)
    text = text.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def patch_websockets(env, node):
    path = node.srcnode().get_abspath()
    name = path.replace("\\", "/")
    if name.endswith("/WebSocketsClient.cpp"):
        _patch(path, "kommOnStatus(", STATUS_OLD, STATUS_NEW, STATUS_DECL)
        _patch(path, "kommOnArm();", ARM_OLD, ARM_NEW, STATUS_DECL)
    elif name.endswith("/WebSockets.cpp"):
        _patch(path, "kommOnClose(", CLOSE_OLD, CLOSE_NEW, CLOSE_DECL)
    return node


env.AddBuildMiddleware(patch_websockets, "*WebSockets*.cpp")
