import http.server
import socketserver
import socket
import os
import json
import time
import urllib.parse
import hashlib
from datetime import datetime

def calculate_file_sha256(filepath):
    try:
        sha256 = hashlib.sha256()
        with open(filepath, "rb") as f:
            while chunk := f.read(65536):
                sha256.update(chunk)
        return sha256.hexdigest()
    except Exception:
        return "unknown"

# Port allocations
WEB_PORT = 5000
CORE_PORT = 8080

# Protocol Constants matching C++ node
MAGIC_B1 = 0x51
MAGIC_B2 = 0x43
VERSION = 0x01

# RBFTP Packet Type IDs
TYPE_PING = 1
TYPE_DATA = 2
TYPE_ACK = 3
TYPE_ERR = 4
TYPE_FILE_START = 5
TYPE_FILE_CHUNK = 6
TYPE_FILE_END = 7

def calculate_checksum(data):
    """16-bit Internet Checksum (RFC 1071) matched to C++ Core parser"""
    checksum_sum = 0
    # Add 16-bit words
    for i in range(0, len(data) - 1, 2):
        word = (data[i] << 8) | data[i + 1]
        checksum_sum += word
    # Handle odd byte
    if len(data) % 2 != 0:
        checksum_sum += (data[-1] << 8)
    # Fold 32-bit sum to 16 bits
    while checksum_sum >> 16:
        checksum_sum = (checksum_sum & 0xFFFF) + (checksum_sum >> 16)
    return (~checksum_sum) & 0xFFFF

def serialize_packet(type_id, payload):
    """Encodes Packet struct into standard RBFTP byte frame"""
    length = len(payload)
    checksum = calculate_checksum(payload) if length > 0 else 0
    
    # 8-Byte Header: Magic(2B) + Version(1B) + Type(1B) + Length(2B) + Checksum(2B)
    header = bytearray([
        MAGIC_B1,
        MAGIC_B2,
        VERSION,
        type_id,
        (length >> 8) & 0xFF,
        length & 0xFF,
        (checksum >> 8) & 0xFF,
        checksum & 0xFF
    ])
    return header + payload

def parse_ack_response(conn_socket):
    """Reads response frame from Core C++ storage socket"""
    try:
        header = conn_socket.recv(8)
        if len(header) != 8:
            return False, "Short response header"
        
        type_id = header[3]
        length = (header[4] << 8) | header[5]
        
        payload = b""
        if length > 0:
            payload = conn_socket.recv(length)
            
        payload_str = payload.decode('utf-8', errors='ignore')
        
        if type_id == TYPE_ACK:
            return True, payload_str
        elif type_id == TYPE_ERR:
            return False, f"Server Error: {payload_str}"
        else:
            return False, f"Unexpected type ID: {type_id}"
    except Exception as e:
        return False, f"Socket error: {str(e)}"

class AetherGatewayHandler(http.server.BaseHTTPRequestHandler):
    
    def log_message(self, format, *args):
        # Override to suppress default console print logging
        pass

    def do_GET(self):
        parsed_url = urllib.parse.urlparse(self.path)
        path = parsed_url.path
        
        if path == "/" or path == "/index.html":
            self.serve_static_file("gateway/static/dashboard.html", "text/html")
        elif path == "/api/files":
            self.handle_get_files()
        elif path == "/api/metrics":
            self.handle_get_metrics()
        else:
            # Fallback pathing
            self.send_error(404, "Page Not Found")

    def do_POST(self):
        parsed_url = urllib.parse.urlparse(self.path)
        path = parsed_url.path
        
        if path == "/api/upload":
            self.handle_post_upload(parsed_url.query)
        elif path == "/api/ping":
            self.handle_post_ping()
        else:
            self.send_error(404, "API endpoint not found")

    def serve_static_file(self, filepath, content_type):
        # Relative file lookup matching execution contexts
        try:
            # Look relative to workspace directory
            full_path = os.path.abspath(filepath)
            if not os.path.exists(full_path):
                # Fallback to current folder
                full_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "static", "dashboard.html"))
                
            with open(full_path, "r", encoding="utf-8") as f:
                content = f.read()
                
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(content.encode("utf-8"))
        except Exception as e:
            self.send_error(500, f"Static asset load failed: {str(e)}")

    def handle_get_files(self):
        """Lists files currently saved in the server's uploads folder"""
        uploads_dir = "uploads"
        files_list = []
        
        if os.path.exists(uploads_dir):
            try:
                for entry in os.scandir(uploads_dir):
                    if entry.is_file():
                        stat = entry.stat()
                        mod_time = datetime.fromtimestamp(stat.st_mtime).strftime('%Y-%m-%d %H:%M:%S')
                        file_hash = calculate_file_sha256(entry.path)
                        files_list.append({
                            "name": entry.name,
                            "size": stat.st_size,
                            "modified": mod_time,
                            "hash": file_hash
                        })
            except Exception as e:
                pass
                
        self.send_json_response(200, files_list)

    def handle_get_metrics(self):
        """Aggregates metrics and log tailing from the daemon node"""
        uploads_dir = "uploads"
        file_count = 0
        total_bytes = 0
        
        if os.path.exists(uploads_dir):
            try:
                for entry in os.scandir(uploads_dir):
                    if entry.is_file():
                        file_count += 1
                        total_bytes += entry.stat().st_size
            except Exception:
                pass
                
        # Tail last 15 server logs
        logs = []
        if os.path.exists("server.log"):
            try:
                with open("server.log", "r", encoding="utf-8") as f:
                    lines = f.readlines()
                    logs = [line.strip() for line in lines[-15:]]
            except Exception:
                pass

        # Check if C++ server is online
        server_online = False
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.5)
            s.connect(("127.0.0.1", CORE_PORT))
            server_online = True
            s.close()
        except Exception:
            pass

        metrics = {
            "status": "Online" if server_online else "Offline",
            "port": CORE_PORT,
            "file_count": file_count,
            "total_bytes": total_bytes,
            "logs": logs
        }
        self.send_json_response(200, metrics)

    def handle_post_ping(self):
        """Proxies ping request to verify C++ node latency"""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(("127.0.0.1", CORE_PORT))
            
            ping_pkt = serialize_packet(TYPE_PING, b"")
            start_time = time.perf_counter()
            s.sendall(ping_pkt)
            
            ok, msg = parse_ack_response(s)
            end_time = time.perf_counter()
            s.close()
            
            if ok:
                rtt = int((end_time - start_time) * 1000)
                self.send_json_response(200, {"status": "success", "rtt": rtt, "msg": msg})
            else:
                self.send_json_response(400, {"status": "error", "message": msg})
        except Exception as e:
            self.send_json_response(500, {"status": "error", "message": f"Core offline: {str(e)}"})

    def handle_post_upload(self, query_string):
        """Translates HTTP POST raw stream into C++ RBFTP chunk sockets"""
        query_params = urllib.parse.parse_qs(query_string)
        filename = query_params.get("filename", ["uploaded_file.bin"])[0]
        
        content_length = int(self.headers.get("Content-Length", 0))
        if content_length == 0:
            self.send_json_response(400, {"status": "error", "message": "Content-Length header missing or 0 bytes."})
            return

        try:
            # 1. Connect to C++ Core Socket Node
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(("127.0.0.1", CORE_PORT))
        except Exception as e:
            self.send_json_response(500, {"status": "error", "message": f"Failed to connect to C++ Storage Core: {str(e)}"})
            return

        try:
            # 2. Transmit FILE_START frame
            # Payload: [size (4B Big-Endian)] + [filename string]
            start_payload = bytearray([
                (content_length >> 24) & 0xFF,
                (content_length >> 16) & 0xFF,
                (content_length >> 8) & 0xFF,
                content_length & 0xFF
            ]) + filename.encode('utf-8')
            
            start_packet = serialize_packet(TYPE_FILE_START, start_payload)
            s.sendall(start_packet)
            
            ok, msg = parse_ack_response(s)
            if not ok:
                s.close()
                self.send_json_response(400, {"status": "error", "message": f"Server rejected start: {msg}"})
                return

            # 3. Stream request body directly into C++ node sockets (in chunks of 4KB)
            bytes_transferred = 0
            chunk_size = 4096
            
            while bytes_transferred < content_length:
                remaining = content_length - bytes_transferred
                read_size = min(chunk_size, remaining)
                
                # Stream binary block from HTTP body
                chunk_data = self.rfile.read(read_size)
                if not chunk_data:
                    break
                
                # Wrap inside RBFTP packet frame
                chunk_packet = serialize_packet(TYPE_FILE_CHUNK, chunk_data)
                s.sendall(chunk_packet)
                
                ok, msg = parse_ack_response(s)
                if not ok:
                    s.close()
                    self.send_json_response(500, {"status": "error", "message": f"Chunk write failed: {msg}"})
                    return
                
                bytes_transferred += len(chunk_data)

            # 4. Transmit FILE_END validation check
            end_packet = serialize_packet(TYPE_FILE_END, b"")
            s.sendall(end_packet)
            
            ok, msg = parse_ack_response(s)
            s.close()

            if ok:
                self.send_json_response(200, {"status": "success", "message": f"Successfully ingested file: {filename}"})
            else:
                self.send_json_response(400, {"status": "error", "message": f"Disk verify failed: {msg}"})
        except Exception as e:
            s.close()
            self.send_json_response(500, {"status": "error", "message": f"Proxy transfer aborted: {str(e)}"})

    def send_json_response(self, status_code, data):
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps(data).encode("utf-8"))

def run():
    handler = AetherGatewayHandler
    # Allow socket address reuse instantly on reload
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", WEB_PORT), handler) as httpd:
        print(f"[AetherFlow] API Gateway running on http://127.0.0.1:{WEB_PORT}")
        httpd.serve_forever()

if __name__ == "__main__":
    run()
