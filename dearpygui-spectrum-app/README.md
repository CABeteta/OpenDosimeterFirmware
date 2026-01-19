# Dear PyGui Spectrum App

This project is a Dear PyGui application designed for reading and processing spectrum data from a serial device. It provides a user-friendly interface with buttons to execute various functions related to spectrum analysis, including reading data, plotting, and saving results.

## Project Structure

```
dearpygui-spectrum-app
├── src
│   ├── main.py          # Entry point for the Dear PyGui application
│   ├── spectrum.py      # Core functionality for spectrum reading and processing
│   └── utils.py         # Utility functions for file operations and data processing
├── requirements.txt      # List of dependencies required for the project
└── README.md             # Documentation for the project
```

## Installation

To set up the project, follow these steps:

1. Clone the repository:
   ```
   git clone <repository-url>
   cd dearpygui-spectrum-app
   ```

2. Install the required dependencies:
   ```
   pip install -r requirements.txt
   ```

## Usage

To run the application, execute the following command:

```
python src/main.py
```

Once the application is running, you will see a window with buttons for each function:

- **Read Spectrum**: Reads data from the specified serial port.
- **Plot Spectrum**: Displays the raw and smoothed spectrum plots.
- **Save Spectrum**: Saves the spectrum data to a CSV file.

## Functionality

- **ReadTTY**: Reads spectrum data from a specified serial port.
- **SmoothSpectrum**: Applies smoothing to the raw spectrum data.
- **SaveSpectrumToCSV**: Saves the spectrum data and smoothed data to a CSV file.
- **LoadSpectrumFromCSV**: Loads spectrum data from a CSV file.
- **PlotSpectrum**: Plots the raw and smoothed spectrum data.
- **PlotSpectrumSingle**: Plots the smoothed spectrum data with raw data points.

## Dependencies

This project requires the following Python packages:

- DearPyGui
- matplotlib
- pyserial
- psutil

Make sure to install these packages using the `requirements.txt` file provided.

## License

This project is licensed under the MIT License. See the LICENSE file for more details.