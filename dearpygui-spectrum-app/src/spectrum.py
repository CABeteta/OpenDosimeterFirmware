import os
import datetime
import dearpygui.dearpygui as dpg
import serial
import psutil
import matplotlib.pyplot as plt
import numpy as np

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