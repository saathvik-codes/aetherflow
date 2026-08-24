import subprocess
import time
import os
import hashlib
import sys
import socket

# Color formatting helpers for clean console output
def print_pass(msg):
    print(f"\033[92m[PASS] {msg}\033[0m")

def print_fail(msg):
    print(f"\033[91m[FAIL] {msg}\033[0m")
    sys.exit(1)

def print_info(msg):
    print(f"[Info] {msg}")

def calculate_sha256(filepath):
    sha256 = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(8192):
            sha256.update(chunk)
    return sha256.hexdigest()

def clean_file(path):
    if os.path.exists(path):
        try:
            os.remove(path)
        except Exception as e:
            print_info(f"Could not delete {path}: {e}")

def main():
    print("====================================================")
    print("      RBFTP END-TO-END AUTOMATED TEST RUNNER       ")
    print("====================================================\n")

    # Step 1: Clean build files
    print_info("Cleaning existing binaries...")
    clean_file("server.exe")
    clean_file("client.exe")
    clean_file("server")
    clean_file("client")
    clean_file("rbf_test_payload.txt")
    clean_file("server.log")
    clean_file("uploads/uploaded_qualcomm_test.txt")

    # Step 2: Compile server and client binaries
    print_info("Compiling C++ codebase...")
    is_windows = os.name == "nt"
    
    server_source = ["src/server.cpp", "src/packet.cpp"]
    client_source = ["src/client.cpp", "src/packet.cpp"]
    
    server_output = "server.exe" if is_windows else "server"
    client_output = "client.exe" if is_windows else "client"
    
    compile_flags = ["-std=c++11", "-Wall", "-O2"]
    
    # Windows Winsock linking
    libs = ["-lws2_32"] if is_windows else ["-lpthread"]
    
    # Run compiler
    server_build_cmd = ["g++"] + compile_flags + server_source + ["-o", server_output] + libs
    client_build_cmd = ["g++"] + compile_flags + client_source + ["-o", client_output] + libs
    
    try:
        subprocess.run(server_build_cmd, check=True)
        print_pass("Server compiled successfully.")
    except subprocess.CalledProcessError as e:
        print_fail(f"Server compilation failed: {e}")
        
    try:
        subprocess.run(client_build_cmd, check=True)
        print_pass("Client compiled successfully.")
    except subprocess.CalledProcessError as e:
        print_fail(f"Client compilation failed: {e}")

    # Step 3: Create a mock input file for binary upload testing
    print_info("Generating mock binary file (10 KB)...")
    mock_input_file = "rbf_test_payload.txt"
    mock_content = b"Qualcomm SW Systems Ingestion Test\n" * 300 # Roughly 10 KB
    with open(mock_input_file, "wb") as f:
        f.write(mock_content)
    
    input_hash = hashlib.sha256(mock_content).hexdigest()
    print_info(f"Test file generated. SHA-256 Hash: {input_hash}")

    # Step 4: Spawn the RBFTP server in background
    print_info("Starting server subprocess on port 8080...")
    server_cmd = [f"./{server_output}"]
    
    server_proc = subprocess.Popen(
        server_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    # Give the server a moment to bind to socket port 8080
    time.sleep(1.5)
    
    if server_proc.poll() is not None:
        # Server crashed immediately
        stdout, stderr = server_proc.communicate()
        print_fail(f"Server failed to start.\nStdout: {stdout}\nStderr: {stderr}")

    # Step 5: Execute client transmission session
    print_info("Executing client simulation subprocess...")
    client_cmd = [f"./{client_output}"]
    
    try:
        # Spawn client in interactive mode with stdin pipe
        client_proc = subprocess.Popen(
            client_cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        
        # Send interactive CLI commands: ping first, then send the file, then exit
        commands = f"ping\nsend {mock_input_file}\nexit\n"
        stdout, stderr = client_proc.communicate(input=commands, timeout=20)
        
        print_info("Client output:\n" + stdout)
        
        if client_proc.returncode == 0:
            print_pass("Client completed session with code 0.")
        else:
            print_fail(f"Client failed with exit code: {client_proc.returncode}\nStderr: {stderr}")
            
    except subprocess.TimeoutExpired:
        client_proc.kill()
        client_proc.communicate()
        print_fail("Client execution timed out (possible deadlock or network hang).")

    # Step 6: Verify file assembly & hash integrity
    print_info("Verifying transfer results...")
    uploaded_file = f"uploads/{mock_input_file}"
    
    if not os.path.exists(uploaded_file):
        print_fail(f"Verification Failed: File was not created at {uploaded_file}")
        
    output_hash = calculate_sha256(uploaded_file)
    print_info(f"Uploaded file SHA-256 Hash: {output_hash}")
    
    if input_hash == output_hash:
        print_pass("Hash comparison matches exactly! File reassembled with 100% integrity.")
    else:
        print_fail(f"Hash mismatch! File is corrupted.\nExpected: {input_hash}\nGot: {output_hash}")

    # Step 7: Verify Checksum error handling via direct raw socket transmission
    print_info("Testing raw corrupted socket transmission...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", 8080))
        # Build bad packet: Magic (QC), Version (1), Type (DATA=2), Len (5), Checksum (0xABCD), Payload (noise)
        bad_packet = b"\x51\x43\x01\x02\x00\x05\xab\xcdnoise"
        s.sendall(bad_packet)
        
        # Read header (8 bytes)
        header = s.recv(8)
        if len(header) == 8:
            resp_type = header[3]
            resp_len = (header[4] << 8) | header[5]
            payload = s.recv(resp_len) if resp_len > 0 else b""
            payload_str = payload.decode("utf-8", errors="ignore")
            
            if resp_type == 4 and "Bad Checksum" in payload_str:
                print_pass("Checksum failure testing confirmed. Server correctly rejected raw corrupted packet.")
            else:
                print_fail(f"Server did not reject bad checksum as expected. Got type: {resp_type}, payload: {payload_str}")
        else:
            print_fail("Failed to receive response header from server for bad packet.")
        s.close()
    except Exception as e:
        print_fail(f"Socket test error: {e}")

    # Step 8: Clean shutdown verification
    print_info("Terminating server process...")
    server_proc.terminate()
    try:
        server_proc.wait(timeout=3)
        print_pass("Server terminated cleanly.")
    except subprocess.TimeoutExpired:
        print_info("Server did not exit on SIGTERM. Forcing kill...")
        server_proc.kill()
        server_proc.wait()
        print_pass("Server killed.")

    # Clean up test artifacts (leaving uploaded file and server log for user review)
    clean_file(mock_input_file)
    
    print("\n====================================================")
    print("          ALL TEST SUITES COMPLETED: [PASS]          ")
    print("====================================================")

if __name__ == "__main__":
    main()
