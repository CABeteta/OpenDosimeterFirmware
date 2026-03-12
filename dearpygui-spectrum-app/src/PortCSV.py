import csv
import argparse
from spectrum import SmoothSpectrum

"""
 The old CSV format was just a dump of the TTY output. It contained a lot of data hard to parse. An example:

    help
    Usage:
        <command> [options]
    Commands:
        P	: Print the dose data log
        S	: Print the current spectrum to serial
        R	: Reset the spectrum to zeroes
        C	: Toggle printing counts per second every second
    > C
    CPS printing enabled
    > 314.00 0.19
    294.00 0.18
    291.00 0.18
    265.00 0.17
    303.70 0.19
    307.31 0.19
    312.00 0.19
    C
    CPS printing disabled
    > R
    > S
    ---Spectrum Log Start
    ADC_BIN,COUNT
    0,78
    1,4
    2,4
    3,5
    [...]
    4095,0

We can print the Counts Per Second (CPS) in the command line, just for info. We can identify it by looking for "CPS printing enabled" and "CPS printing disabled" lines.
The spectrum data is between the "---Spectrum Log Start" and "---Spectrum Log End" lines, with a header line "ADC_BIN,COUNT". Each subsequent line contains the ADC bin number and the count for that bin, separated by a comma.


As for the output format, this is an example:

Channel,Raw,Smoothed
0,0,0
1,0,0
2,0,0
3,0,0
4,0,0
5,0,0
6,0,0
7,0,0
8,0,0
9,0,0
[...]
4095,0,0

 The "Smoothed" column uses SmoothSpectrum from spectrum.py to apply a moving average filter to the raw spectrum data, with a window size of 20.
 """

def import_old_csv(input_file_path="/home/cabellan/Code/Med/OpenDosimeterFirmware/Data/SpectrumTecnetium_05Cm.csv", output_file_path="/home/cabellan/Code/Med/OpenDosimeterFirmware/Data/new/SpectrumTecnetium_05Cm_new.csv"):
    try:
        data = []
        reading_cps = False
        reading_spectrum = False
        spectrum_found = False
        with open(input_file_path, mode='r') as file:
            first_non_empty_line = None
            for raw_line in file:
                stripped_line = raw_line.strip()
                if not stripped_line:
                    continue
                first_non_empty_line = stripped_line
                break
            if first_non_empty_line:
                normalized_header = first_non_empty_line.replace(" ", "").lower()
                if normalized_header.startswith("channel,raw,smoothed"):
                    print(f"Warning: input file '{input_file_path}' appears to already be in new format. Skipping re-import.")
                    return None
            file.seek(0)
            for raw_line in file:
                line = raw_line.rstrip('\n')
                if not line: # skip empty lines
                    continue

                # CPS markers
                if "CPS printing enabled" in line:
                    reading_cps = True
                    print("CPS present:")
                    continue
                if "CPS printing disabled" in line:
                    if reading_cps:
                        print("CPS ended.")
                    reading_cps = False
                    continue
                if reading_cps:
                    print(line)

                # Spectrum markers
                if "---Spectrum Log Start" in line:
                    reading_spectrum = True
                    spectrum_found = True
                    print("Spectrum data found:")
                    continue
                if "---Spectrum Log End" in line:
                    if reading_spectrum:
                        print("Spectrum data ended.")
                    reading_spectrum = False
                    # Keep scanning in case more useful data appears later
                    continue

                # If we're inside the spectrum block, parse CSV-like lines
                if reading_spectrum:
                    # Skip header line if present
                    if line.strip() == "ADC_BIN,COUNT":
                        continue
                    parts = [p.strip() for p in line.split(',')]
                    if len(parts) < 2:
                        continue
                    try:
                        adc_bin = int(parts[0])
                        count = int(parts[1])
                        data.append((adc_bin, count))
                    except ValueError:
                        # ignore non-numeric lines inside spectrum block
                        continue

        if not spectrum_found or len(data) == 0:
            print("No spectrum data found in the file.")
            return None
    except Exception as e:
        print(f"Error importing CSV: {str(e)}")
        return None
    
    smoothed_data = SmoothSpectrum([count for _, count in data], window_size=20)
    # If output_file_path is not provided, we can generate it by appending "_new.csv" to the input file name.
    if not output_file_path:
        output_file_path = input_file_path.rsplit('.', 1)[0] + "_new.csv"

    # Now we write the new format data to a new CSV file with a header "ADC_BIN,COUNT".
    try:
        with open(output_file_path, mode='w', newline='') as file:
            writer = csv.writer(file)
            writer.writerow(["Channel", "Raw", "Smoothed"])
            for i, (adc_bin, count) in enumerate(data):
                writer.writerow([i, count, smoothed_data[i]])
        print(f"Data successfully imported and saved to {output_file_path}.")
    except Exception as e:
        print(f"Error writing new format CSV: {str(e)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Import old CSV format and convert to new format.")
    parser.add_argument("input_file", help="Path to the old CSV file to import.")
    parser.add_argument("--output_file", help="Path to save the new CSV file. If not provided, it will be generated based on the input file name.")
    args = parser.parse_args()
    import_old_csv(args.input_file, args.output_file)
