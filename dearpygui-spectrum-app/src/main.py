import dearpygui.dearpygui as dpg
import datetime
from spectrum import ReadTTY, SmoothSpectrum, SaveSpectrumToCSV, FindRaspberryPiTTY, WriteTTY

spectrum_data = []
smoothed_data = []

def reset_spectrum_callback():
    print("Resetting spectrum data...")

    try:
        port=FindRaspberryPiTTY()
        print(f"Raspberry Pi TTY port found: {port}")
        if not port:
            dpg.set_value("status_text", "Error: Raspberry Pi port not found.")
            return
        WriteTTY(port=port, data="R\n")
        dpg.set_value("status_text", "Spectrum reset command sent to TTY.")
    except Exception as e:
        dpg.set_value("status_text", f"Error: {str(e)}")


def read_spectrum_callback():
    print("Reading spectrum data from TTY...")
    global spectrum_data, smoothed_data
    try:

        port = FindRaspberryPiTTY()
        print(f"Raspberry Pi TTY port found: {port}")
        
        if not port:
            dpg.set_value("status_text", "Error: Raspberry Pi port not found.")
            return
        spectrum_data = ReadTTY(port=port)
        smoothed_data = SmoothSpectrum(spectrum_data, window_size=20)
        for i in range(10):
            spectrum_data[i] = 0
            smoothed_data[i] = 0
        dpg.set_value("status_text", "Spectrum data read from TTY.")
    except Exception as e:
        dpg.set_value("status_text", f"Error: {str(e)}")

def plot_spectrum_callback():
    if dpg.does_item_exist("raw_series") and dpg.does_item_exist("smoothed_series"):
        if spectrum_data and smoothed_data:
            dpg.set_value("raw_series", [list(range(len(spectrum_data))), spectrum_data])
            dpg.set_value("smoothed_series", [list(range(len(smoothed_data))), smoothed_data])
            # Autoscale axes using axis tags
            dpg.fit_axis_data("x_axis")
            dpg.fit_axis_data("y_axis")
            dpg.set_value("status_text", "Spectrum plotted.")
        else:
            dpg.set_value("status_text", "No spectrum data to plot.")
    else:
        dpg.set_value("status_text", "Plot not initialized yet.")

def save_csv_callback():
    try:
        if spectrum_data and smoothed_data:
            timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"spectrum_{timestamp}.csv"
            SaveSpectrumToCSV(spectrum_data, smoothed_data, filename)
            dpg.set_value("status_text", f"Spectrum data saved to {filename}.")
        else:
            dpg.set_value("status_text", "No spectrum data to save.")
    except Exception as e:
        dpg.set_value("status_text", f"Error: {str(e)}")

def file_dialog_callback(sender, app_data):
    global spectrum_data, smoothed_data
    try:
        from spectrum import LoadSpectrumFromCSV
        filename = app_data['file_path_name']
        if filename:
            spectrum_data, smoothed_data = LoadSpectrumFromCSV(filename)
            dpg.set_value("status_text", f"Spectrum data loaded from {filename}.")
            plot_spectrum_callback()
        else:
            dpg.set_value("status_text", "No file selected.")
    except Exception as e:
        dpg.set_value("status_text", f"Error: {str(e)}")

def load_spectrum_callback():
    dpg.show_item("file_dialog_id")

def resize_callback(sender, app_data):
    # Get new viewport size
    width, height = dpg.get_viewport_client_width(), dpg.get_viewport_client_height()
    # Adjust window, child window, and plot sizes
    dpg.configure_item("main_window", width=width, height=height)
    dpg.configure_item("plot_child", width=width, height=height-60)  # leave space for buttons/text
    dpg.configure_item("plot", width=width-40, height=height-100)    # leave space for legend/axes

def setup_ui():
    dpg.create_context()
    dpg.create_viewport(title='Spectrum Reader', width=900, height=700)
    
    with dpg.window(label="Spectrum App", width=900, height=700, tag="main_window"):
        with dpg.group(horizontal=True):
            dpg.add_button(label="Read Spectrum", callback=read_spectrum_callback)
            dpg.add_button(label="Load Spectrum", callback=load_spectrum_callback)
            dpg.add_button(label="Save CSV", callback=save_csv_callback)
            dpg.add_button(label="Plot Spectrum", callback=plot_spectrum_callback)
            dpg.add_button(label="Reset Spectrum", callback=reset_spectrum_callback)
        dpg.add_text("", tag="status_text")
        with dpg.child_window(width=900, height=600, tag="plot_child"):
            with dpg.plot(label="Spectrum Plot", tag="plot", width=860, height=560):
                dpg.add_plot_legend(location=dpg.mvPlot_Location_NorthEast)
                x_axis = dpg.add_plot_axis(dpg.mvXAxis, label="Channel", tag="x_axis")
                y_axis = dpg.add_plot_axis(dpg.mvYAxis, label="Counts", tag="y_axis")
                dpg.add_line_series([0], [0], label="Raw Spectrum", tag="raw_series", parent=y_axis)
                dpg.add_line_series([0], [0], label="Smoothed Spectrum", tag="smoothed_series", parent=y_axis)
        # Add file dialog (hidden by default)
        
        with dpg.file_dialog(
            directory_selector=False,
            show=False,
            callback=file_dialog_callback,
            tag="file_dialog_id",
            width=700,
            height=500,
        ):
            # Add file extension filters properly
            dpg.add_file_extension(".csv", color=(0, 255, 0, 255))
            dpg.add_file_extension(".*")  # Optional: allow all files
    # Set up viewport resize callback
    dpg.set_viewport_resize_callback(resize_callback)
    dpg.setup_dearpygui()
    dpg.show_viewport()
    dpg.start_dearpygui()
    dpg.destroy_context()

if __name__ == "__main__":
    setup_ui()