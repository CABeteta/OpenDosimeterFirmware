import os
import datetime
import dearpygui.dearpygui as dpg
import serial
import psutil
import matplotlib.pyplot as plt
import numpy as np
import time

def WriteTTY(port='/dev/ttyACM0', baudrate=9600, timeout=5, data="Hello, Raspberry Pi!"):
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
        ser.write(data.encode())
        print(f"Data written to {port}: {data}")

def ReadCPS(port='/dev/ttyACM0', data="C\n", baudrate=9600, write_timeout=5, read_timeout=1):
    """Write to a port and return any lines received within a timeout.

    The write uses :func:`WriteTTY` to leverage its safety checks.  After the
    data is written the port is opened again for up to ``read_timeout``
    seconds to collect any response lines, which are joined by ``\n``.
    """
    # perform the write with existing helper. writes a C to read the cps
    WriteTTY(port=port, baudrate=baudrate, timeout=write_timeout, data=data)
    responses = []
    start = time.time()
    with serial.Serial(port, baudrate, timeout=0.5) as ser:
        while time.time() - start < read_timeout:
            line = ser.readline().decode(errors='ignore').strip()
            if line:
                 #write again a C to stop the counting
                WriteTTY(port=port, baudrate=baudrate, timeout=write_timeout, data=data)
                responses=line
                return responses
   
    
   
    
    
def ReadTTY(port='/dev/ttyACM0', baudrate=9600, timeout=5):
    spectrum = []
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
        while True:
            line = ser.readline().decode().strip()
            if line == "---Spectrum Log Start":
                break
        header = ser.readline()
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
        while True:
            line = ser.readline().decode().strip()
            if line == "---Spectrum Log End":
                break
    return spectrum

def SmoothSpectrum(spectrum, window_size=10):
    smoothed = []
    for i in range(len(spectrum)):
        window = spectrum[max(0, i - window_size // 2):min(len(spectrum), i + window_size // 2 + 1)]
        smoothed.append(sum(window) // len(window))
    return smoothed

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

def SaveSpectrumToCSV(spectrum, smoothed, filename):
    with open(filename, 'w') as f:
        f.write("Channel,Raw,Smoothed\n")
        for i in range(4096):
            f.write(f"{i},{spectrum[i]},{smoothed[i]}\n")

def PlotSpectrum(spectrum, smoothed):
    plt.figure(figsize=(12, 6))
    x = range(4096)
    plt.subplot(2, 1, 1)
    plt.plot(x, spectrum, label='Raw Spectrum')
    plt.title('Raw Spectrum')
    plt.xlabel('Channel')
    plt.ylabel('Counts')
    plt.xlim(0, 4096)
    plt.subplot(2, 1, 2)
    plt.plot(x, smoothed, label='Smoothed Spectrum', color='orange')
    plt.title('Smoothed Spectrum')
    plt.xlabel('Channel')
    plt.ylabel('Counts')
    plt.xlim(0, 4096)
    plt.tight_layout()
    plt.show()

def PlotSpectrumSingle(spectrum, smoothed):
    plt.figure(figsize=(12, 6))
    x = range(4096)
    plt.plot(x, smoothed, label='Smoothed Spectrum', color='orange')
    plt.scatter(x, spectrum, label='Raw Spectrum', color='blue', marker='x', s=10)
    plt.title('Spectrum')
    plt.xlabel('Channel')
    plt.ylabel('Counts')
    plt.xlim(0, 4096)
    plt.tight_layout()
    plt.show()

def FindAllRaspberryPiTTYs():
    """Return a list of all serial ports that look like Raspberry Pi devices.

    This uses the same heuristics as :func:`FindRaspberryPiTTY` but does not
    stop after the first match. An empty list is returned if no ports are found.
    """
    try:
        from serial.tools import list_ports
    except ImportError:
        raise ImportError("pyserial is required. Install it with: pip install pyserial")

    rpi_identifiers = [
        'Raspberry Pi',
        'Pico',
        'CH340',
        'CP210x',
        'FTDI',
        'arduino',
    ]

    ports = list_ports.comports()
    matches = []

    for port in ports:
        desc_lower = (port.description or '').lower()
        manufacturer_lower = (port.manufacturer or '').lower()

        for identifier in rpi_identifiers:
            if identifier.lower() in desc_lower or identifier.lower() in manufacturer_lower:
                matches.append(port.device)
                break
    # If no identified matches, return an empty list
    if not matches:
        return []

    return matches