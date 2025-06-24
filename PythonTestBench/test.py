import SerialPortManager
import time

def communicate_with_device(spm, command='help'):
    try:
        with spm.open_port() as ser:
            print(f"Opened {port_name}")
            ser.write((command + '\n').encode())
            print(f"Sent command: {command}")
            # Read lines until timeout or no more data
            lines = []
            start_time = time.time()
            for _ in range(100):
                if time.time() - start_time > 10:
                    break
                line = ser.readline().decode().strip()
                if not line:
                    break
                lines.append(line)
            response = "\n".join(lines) if lines else None # Now 'lines' contains all the lines read
            if response:
                # Print each line separately
                for line in lines:
                    print(line)
            else:
                print("No response received from the device.")
            print("Communication complete.")

    except Exception as e:
        print(f"Failed to open {port_name}: {e}")

if __name__ == "__main__":
    spm = SerialPortManager.SerialPortManager()
    ports = spm.list_ports_with_details()

    for idx, port in enumerate(ports):
        print(f"{idx}: {port['device']} - {port['description']}")

    target = input("Enter port number: ").strip()
    if target.isdigit() and int(target) < len(ports):
        port_name = ports[int(target)]['device']
    else:
        print("Invalid port number. Please try again.")
        exit(1)
    print(f"Target device: {port_name}")
    spm.select_port(int(target))
    spm.set_baudrate(9600)
    spm.set_timeout(1)
    print(f"Selected port: {spm.get_selected_port()}")
    print("Starting communication...")
    communicate_with_device(spm)
    print("Done.")