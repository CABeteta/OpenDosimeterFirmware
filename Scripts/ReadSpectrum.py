import os
import sys
import datetime

def get_default_port():
    if sys.platform.startswith("win"):
        return "COM3"  # Or prompt user for correct COM port
    else:
        return "/dev/ttyACM0"

def ReadTTY(port=None, baudrate=9600, timeout=5):
    import serial
    import psutil
    if port is None:
        port = get_default_port()
    spectrum = []
    if sys.platform.startswith("win"):
        # On Windows, os.path.exists does not work for COM ports
        # Try opening the port and catch exceptions
        try:
            ser = serial.Serial(port, baudrate, timeout=timeout)
            ser.close()
        except serial.SerialException:
            raise FileNotFoundError(f"Serial port {port} not found or cannot be opened.")
    else:
        if not os.path.exists(port):
            raise FileNotFoundError(f"Serial port {port} not found.")
    for proc in psutil.process_iter(['pid', 'name', 'open_files']):
        try:
            files = proc.info['open_files']
            if files:
                for f in files:
                    if f.path == port:
                        raise RuntimeError(f"Serial port {port} is already in use by process {proc.info['name']} (PID {proc.info['pid']}).")
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    with serial.Serial(port, baudrate, timeout=timeout) as ser:
        ser.write(b'S\n')
        # Wait for start marker
        while True:
            line = ser.readline().decode().strip()
            if line == "---Spectrum Log Start":
                break
        # Skip header
        header = ser.readline()
        # Read 4096 lines
        for _ in range(4096):
            line = ser.readline().decode().strip()
            parts = line.split(',')
            if len(parts) == 2:
                try:
                    value = int(parts[1])
                    spectrum.append(value)
                except ValueError:
                    spectrum.append(0)
            else:
                spectrum.append(0)
        # Optionally, wait for end marker
        while True:
            line = ser.readline().decode().strip()
            if line == "---Spectrum Log End":
                break
    return spectrum

def PlotSpectrum(spectrum, smoothed, filename):
    import matplotlib.pyplot as plt
    plt.figure(figsize=(12, 12))
    x = range(4096)
    plt.subplot(2, 1, 1)
    plt.plot(x, values, label='Raw Spectrum')
    plt.title('Raw Spectrum')
    plt.xlabel('Channel')
    plt.ylabel('Counts')
    plt.xlim(0, 4096)
    plt.xticks(range(0, 4097, 512))  # Major ticks every 512
    plt.minorticks_on()
    plt.gca().set_xticks(range(0, 4097, 128), minor=True)  # Minor ticks every 128
    plt.grid(which='major', linestyle='-', linewidth=0.8, color='gray')
    plt.grid(which='minor', linestyle=':', linewidth=0.5, color='lightgray')
    plt.legend()
    plt.subplot(2, 1, 2)
    plt.plot(x, smoothed, label='Smoothed Spectrum', color='orange')
    plt.title('Smoothed Spectrum')
    plt.xlabel('Channel')
    plt.ylabel('Counts')
    plt.xlim(0, 4096)
    plt.xticks(range(0, 4097, 512))  # Major ticks every 512
    plt.minorticks_on()
    plt.gca().set_xticks(range(0, 4097, 128), minor=True)  # Minor ticks every 128
    plt.grid(which='major', linestyle='-', linewidth=0.8, color='gray')
    plt.grid(which='minor', linestyle=':', linewidth=0.5, color='lightgray')
    plt.legend()
    plt.tight_layout()
    # We also save the plot to a PNG file with the same timestamp
    plot_filename = f"spectrum_{timestamp}.png"
    plt.savefig(plot_filename)
    plt.show()
    print(f"Spectrum data saved to {filename} and plot saved to {plot_filename}.")

def PlotSpectrumSingle(spectrum, smoothed, filename):
    import matplotlib.pyplot as plt
    x = range(4096)
    plt.figure(figsize=(12, 6))
    plt.plot(x, smoothed, label='Smoothed Spectrum', color='orange')
    plt.scatter(x, spectrum, label='Raw Spectrum', color='blue', marker='x', s=10)
    plt.title('Spectrum')
    plt.xlabel('Channel')
    plt.ylabel('Counts')
    plt.xlim(0, 4096)
    plt.xticks(range(0, 4097, 512))
    plt.minorticks_on()
    plt.gca().set_xticks(range(0, 4097, 128), minor=True)
    plt.grid(which='major', linestyle='-', linewidth=0.8, color='gray')
    plt.grid(which='minor', linestyle=':', linewidth=0.5, color='lightgray')
    plt.legend()
    plt.tight_layout()
    plot_filename = f"spectrum_single_{timestamp}.png"
    plt.savefig(plot_filename)
    plt.show()
    print(f"Spectrum data saved to {filename} and single plot saved to {plot_filename}.")

def SaveSpectrumToCSV(spectrum, smoothed, filename):
    with open(filename, 'w') as f:
        f.write("Channel,Raw,Smoothed\n")
        for i in range(4096):
            f.write(f"{i},{spectrum[i]},{smoothed[i]}\n")

def LoadSpectrumFromCSV(filename):
    spectrum = []
    smoothed = []
    with open(filename, 'r') as f:
        next(f)  # Skip header
        for line in f:
            parts = line.strip().split(',')
            if len(parts) == 3:
                try:
                    spectrum.append(int(parts[1]))
                    smoothed.append(int(parts[2]))
                except ValueError:
                    spectrum.append(0)
                    smoothed.append(0)
            else:
                spectrum.append(0)
                smoothed.append(0)
    return spectrum, smoothed

def SmoothSpectrum(spectrum, window_size=10):
    smoothed = []
    for i in range(len(spectrum)):
        window = spectrum[max(0, i - window_size // 2):min(len(spectrum), i + window_size // 2 + 1)]
        smoothed.append(sum(window) // len(window))
    return smoothed

if __name__ == "__main__":
    values = ReadTTY()
    smoothed = SmoothSpectrum(values, window_size=20)
    # We set to zero the first 5 bins as they are often noisy and affect the plot scale
    for i in range(10):
        values[i] = 0
        smoothed[i] = 0
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"spectrum_{timestamp}.csv"
    SaveSpectrumToCSV(values, smoothed, filename)
    PlotSpectrumSingle(values, smoothed, filename)




