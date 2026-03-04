# We loop on all CSV files in "/home/cabellan/Code/Med/OpenDosimeterFirmware/Data" and run PortCSV.py on each of them, saving the output to a new file with "_new.csv" appended to the original file name.
# This is a test to make sure that PortCSV.py can handle all the different formats of CSV files that we have generated from the Raspberry Pi logs.

import os

file_list = os.listdir("/home/cabellan/Code/Med/OpenDosimeterFirmware/Data")
csv_files = [f for f in file_list if f.endswith(".csv")]

for csv_file in csv_files:
    input_path = os.path.join("/home/cabellan/Code/Med/OpenDosimeterFirmware/Data", csv_file)
    output_path = os.path.join("/home/cabellan/Code/Med/OpenDosimeterFirmware/Data/new", csv_file.replace(".csv", "_new.csv"))
    print(f"Processing {input_path}...")
    os.system(f"python3 /home/cabellan/Code/Med/OpenDosimeterFirmware/dearpygui-spectrum-app/src/PortCSV.py {input_path} --output_file {output_path}")

