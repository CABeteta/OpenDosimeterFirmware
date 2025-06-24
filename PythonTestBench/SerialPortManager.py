import serial
import serial.tools.list_ports

class SerialPortManager:
    def __init__(self):
        self.baudrate = 9600
        self.timeout = 1
        self.ports = []
        self.selected_port = None
        self.update_ports()

    def set_baudrate(self, baudrate):
        if isinstance(baudrate, int) and baudrate > 0:
            self.baudrate = baudrate
        else:
            raise ValueError("Baudrate must be a positive integer.")
        
    def set_timeout(self, timeout):
        if isinstance(timeout, (int, float)) and timeout >= 0:
            self.timeout = timeout
        else:
            raise ValueError("Timeout must be a non-negative number.")
        
    def update_ports(self):
        self.ports = list(serial.tools.list_ports.comports())

    def list_ports(self):
        return [port.device for port in self.ports]

    def list_ports_with_details(self):
        details = []
        for i, port in enumerate(self.ports):
            manufacturer = port.manufacturer or "Unknown"
            description = port.description or "No description"
            details.append({
                "index": i,
                "device": port.device,
                "manufacturer": manufacturer,
                "description": description
            })
        return details

    def select_port(self, index):
        if 0 <= index < len(self.ports):
            self.selected_port = self.ports[index].device
            return True
        return False

    def get_selected_port(self):
        return self.selected_port

    def open_port(self):
        if not self.selected_port:
            raise ValueError("No port has been selected.")
        if not self.baudrate or not self.timeout:
            raise ValueError("Baudrate or timeout is not set.")
        return serial.Serial(port=self.selected_port, baudrate=self.baudrate, timeout=self.timeout)