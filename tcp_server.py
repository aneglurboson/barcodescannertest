
import socket
import struct

SERVER_HOST = "0.0.0.0"
SERVER_PORT = 5000

def configure_keepalive(client):
    client.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)

    # Windows: detect an abruptly powered-off Pico.
    try:
        client.ioctl(
            socket.SIO_KEEPALIVE_VALS,
            struct.pack("III", 1, 5000, 2000)
        )
    except (AttributeError, OSError):
        pass

def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    server.bind((SERVER_HOST, SERVER_PORT))
    server.listen(5)

    print("[SERVER] Started successfully")
    print(f"[SERVER] Listening on port {SERVER_PORT}")

    while True:
        client = None

        try:
            print("[WAITING] Waiting for Pico connection...")

            client, address = server.accept()
            configure_keepalive(client)

            print(f"[CONNECTED] Device: {address[0]}")

            while True:
                try:
                    data = client.recv(1024)

                    if not data:
                        print("[DISCONNECTED] Pico closed the connection.")
                        break

                    message = data.decode("utf-8", errors="replace")
                    print("[RECEIVED]", message, end="")

                except socket.timeout:
                    continue

        except KeyboardInterrupt:
            print("\n[SERVER] Stopping server...")
            break

        except OSError as error:
            print("[SERVER ERROR]", error)

        finally:
            if client:
                try:
                    client.close()
                except OSError:
                    pass

            print("[SERVER] Ready for the next connection.")

    server.close()

if __name__ == "__main__":
    main()
