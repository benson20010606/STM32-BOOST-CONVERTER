import serial
import serial.tools.list_ports
import tkinter as tk
from collections import deque
import re


# ============================================================
# Configuration
# ============================================================

BAUD_RATE = 115200

# STM32 ADC / controller rate
CONTROL_FREQ_HZ = 5000.0

# Display the latest 10 seconds
PLOT_WINDOW_SEC = 10.0

# UART telemetry is approximately 50 Hz
MAX_POINTS = 600


# ============================================================
# Serial port detection
# ============================================================

def find_stm32_port():
    """
    Try to automatically find the STM32 Nucleo
    ST-LINK Virtual COM Port.
    """

    ports = list(serial.tools.list_ports.comports())

    print("Available serial ports:")

    for port in ports:
        print(
            f"  {port.device}: "
            f"{port.description}"
        )

    keywords = [
        "STMicroelectronics",
        "STLink",
        "ST-LINK",
        "STM32",
        "Virtual COM"
    ]

    for port in ports:
        text = (
            port.description + " " +
            port.manufacturer
            if port.manufacturer
            else port.description
        )

        for keyword in keywords:
            if keyword.lower() in text.lower():
                print(
                    f"\nSTM32 detected: "
                    f"{port.device}"
                )

                return port.device

    return None


# ============================================================
# Serial initialization
# ============================================================

SERIAL_PORT = find_stm32_port()

if SERIAL_PORT is None:
    print("\nSTM32 COM port was not detected.")
    print("Please set SERIAL_PORT manually.")

    # --------------------------------------------------------
    # Change this if automatic detection fails.
    # Example:
    #
    # SERIAL_PORT = "COM5"
    # --------------------------------------------------------
    SERIAL_PORT = "COM5"


print(f"\nOpening {SERIAL_PORT} at {BAUD_RATE} baud...")


try:
    ser = serial.Serial(
        SERIAL_PORT,
        BAUD_RATE,
        timeout=0
    )

except serial.SerialException as e:
    print("\nFailed to open serial port:")
    print(e)
    raise SystemExit


ser.reset_input_buffer()

print("Serial port opened successfully.")


# ============================================================
# Data buffers
# ============================================================

time_data = deque(maxlen=MAX_POINTS)
vout_data = deque(maxlen=MAX_POINTS)
duty_data = deque(maxlen=MAX_POINTS)


latest_vout = 0.0
latest_duty = 0.0
latest_error = 0
latest_ovp = 0
latest_fb_fault = 0

# Sample counter used as the local t = 0 reference.
# Pressing "Reset Plot" clears the graph and makes the
# next received sample become the new time origin.
time_origin_count = None


# ============================================================
# UART parser
# ============================================================

def parse_csv(line):
    """
    Supports:

    count,vout_mv,error_mv,duty_permille,ovp,fb_fault

    Example:

    12500,7012,-12,298,0,0
    """

    fields = line.split(",")

    if len(fields) != 6:
        return None

    try:
        sample_count = int(fields[0])
        vout_mv = int(fields[1])
        error_mv = int(fields[2])
        duty_permille = int(fields[3])
        ovp = int(fields[4])
        fb_fault = int(fields[5])

        return (
            sample_count,
            vout_mv,
            error_mv,
            duty_permille,
            ovp,
            fb_fault
        )

    except ValueError:
        return None


def parse_text_format(line):
    """
    Supports the STM32 format currently used:

    vout=7012 mV, err=-12 mV,
    duty=29.8%, ovp=0,
    fb_fault=0, count=12500
    """

    pattern = (
        r"vout=(\d+)\s*mV,\s*"
        r"err=(-?\d+)\s*mV,\s*"
        r"duty=(\d+)\.(\d+)%,\s*"
        r"ovp=(\d+),\s*"
        r"fb_fault=(\d+),\s*"
        r"count=(\d+)"
    )

    match = re.search(pattern, line)

    if match is None:
        return None

    vout_mv = int(match.group(1))
    error_mv = int(match.group(2))

    duty_integer = int(match.group(3))
    duty_decimal = int(match.group(4))

    duty_permille = (
        duty_integer * 10 +
        duty_decimal
    )

    ovp = int(match.group(5))
    fb_fault = int(match.group(6))

    sample_count = int(match.group(7))

    return (
        sample_count,
        vout_mv,
        error_mv,
        duty_permille,
        ovp,
        fb_fault
    )


def parse_line(line):
    """
    Automatically support both CSV and
    human-readable STM32 telemetry formats.
    """

    result = parse_csv(line)

    if result is not None:
        return result

    return parse_text_format(line)


# ============================================================
# GUI
# ============================================================

root = tk.Tk()

root.title("STM32 Boost Converter Monitor")

root.geometry("1000x750")


# ============================================================
# Status display
# ============================================================

status_frame = tk.Frame(root)

status_frame.pack(
    fill=tk.X,
    padx=10,
    pady=10
)


vout_label = tk.Label(
    status_frame,
    text="Vout: -- V",
    font=("Arial", 18)
)

vout_label.pack(
    side=tk.LEFT,
    padx=20
)


duty_label = tk.Label(
    status_frame,
    text="Duty: -- %",
    font=("Arial", 18)
)

duty_label.pack(
    side=tk.LEFT,
    padx=20
)


error_label = tk.Label(
    status_frame,
    text="Error: -- mV",
    font=("Arial", 18)
)

error_label.pack(
    side=tk.LEFT,
    padx=20
)


fault_label = tk.Label(
    status_frame,
    text="FAULT: NONE",
    font=("Arial", 18)
)

fault_label.pack(
    side=tk.LEFT,
    padx=20
)


def reset_plot():
    """Clear plot history and restart the displayed time from 0 s."""

    global time_origin_count
    global latest_vout
    global latest_duty
    global latest_error

    time_data.clear()
    vout_data.clear()
    duty_data.clear()

    # The next valid UART packet becomes t = 0.
    time_origin_count = None

    latest_vout = 0.0
    latest_duty = 0.0
    latest_error = 0

    voltage_canvas.delete("all")
    duty_canvas.delete("all")

    print("Plot reset.")


reset_button = tk.Button(
    status_frame,
    text="Reset Plot",
    font=("Arial", 12),
    command=reset_plot
)

reset_button.pack(
    side=tk.RIGHT,
    padx=20
)


# ============================================================
# Voltage plot
# ============================================================

voltage_canvas = tk.Canvas(
    root,
    width=1080,
    height=600,
    bg="white"
)

voltage_canvas.pack(
    padx=10,
    pady=5
)


# ============================================================
# Duty plot
# ============================================================

duty_canvas = tk.Canvas(
    root,
    width=1080,
    height=200,
    bg="white"
)

duty_canvas.pack(
    padx=10,
    pady=5
)


# ============================================================
# Plot helper
# ============================================================

def draw_plot(
    canvas,
    time_values,
    data_values,
    y_min,
    y_max,
    title,
    unit,
    reference=None,
    y_tick_step=None
):
    """
    Draw a realtime line graph using only tkinter Canvas.
    No NumPy or Matplotlib is required.
    """

    canvas.delete("all")

    width = int(canvas["width"])
    height = int(canvas["height"])

    left = 70
    right = width - 20
    top = 30
    bottom = height - 40

    plot_width = right - left
    plot_height = bottom - top


    # --------------------------------------------------------
    # Title
    # --------------------------------------------------------

    canvas.create_text(
        width / 2,
        15,
        text=title,
        font=("Arial", 12, "bold")
    )


    # --------------------------------------------------------
    # Axes
    # --------------------------------------------------------

    canvas.create_line(
        left,
        top,
        left,
        bottom
    )

    canvas.create_line(
        left,
        bottom,
        right,
        bottom
    )


    # --------------------------------------------------------
    # Y-axis labels and horizontal grid
    # --------------------------------------------------------

    if y_tick_step is None:
        y_tick_step = (y_max - y_min) / 5.0

    value = y_min

    while value <= y_max + 1e-6:

        ratio = (
            value - y_min
        ) / (
            y_max - y_min
        )

        y = (
            bottom -
            ratio * plot_height
        )

        canvas.create_line(
            left,
            y,
            right,
            y,
            fill="#dddddd"
        )

        canvas.create_text(
            left - 10,
            y,
            text=f"{value:.1f}",
            anchor="e"
        )

        value += y_tick_step


    canvas.create_text(
        15,
        height / 2,
        text=unit,
        angle=90
    )


    # --------------------------------------------------------
    # No data yet
    # --------------------------------------------------------

    if len(time_values) < 2:
        return


    current_time = time_values[-1]

    time_start = max(
        0.0,
        current_time - PLOT_WINDOW_SEC
    )

    time_end = max(
        PLOT_WINDOW_SEC,
        current_time
    )


    # --------------------------------------------------------
    # X-axis time labels
    # --------------------------------------------------------

    for i in range(6):

        ratio = i / 5

        time_value = (
            time_start +
            ratio *
            (time_end - time_start)
        )

        x = (
            left +
            ratio * plot_width
        )

        canvas.create_text(
            x,
            bottom + 20,
            text=f"{time_value:.1f}s"
        )


    # --------------------------------------------------------
    # Reference line
    # --------------------------------------------------------

    if reference is not None:

        if y_min <= reference <= y_max:

            ratio = (
                reference - y_min
            ) / (
                y_max - y_min
            )

            y = (
                bottom -
                ratio * plot_height
            )

            canvas.create_line(
                left,
                y,
                right,
                y,
                fill="red",
                dash=(5, 5)
            )

            canvas.create_text(
                right - 5,
                y - 10,
                text=f"Reference {reference:.1f}",
                anchor="e",
                fill="red"
            )


    # --------------------------------------------------------
    # Convert data to Canvas coordinates
    # --------------------------------------------------------

    points = []

    for t, value in zip(
        time_values,
        data_values
    ):

        if t < time_start:
            continue


        x_ratio = (
            t - time_start
        ) / (
            time_end - time_start
        )


        y_ratio = (
            value - y_min
        ) / (
            y_max - y_min
        )


        x = (
            left +
            x_ratio * plot_width
        )


        y = (
            bottom -
            y_ratio * plot_height
        )


        # Prevent graph from drawing outside
        # the visible plotting area.

        y = max(
            top,
            min(bottom, y)
        )


        points.extend([x, y])


    # --------------------------------------------------------
    # Draw line
    # --------------------------------------------------------

    if len(points) >= 4:

        canvas.create_line(
            *points,
            fill="blue",
            width=2
        )


# ============================================================
# Serial polling
# ============================================================

def read_serial():

    global latest_vout
    global latest_duty
    global latest_error
    global latest_ovp
    global latest_fb_fault
    global time_origin_count


    # Limit number of packets processed per GUI cycle
    # to prevent the interface from becoming unresponsive.

    packet_count = 0

    while (
        ser.in_waiting > 0 and
        packet_count < 100
    ):

        packet_count += 1


        try:

            raw_line = ser.readline()

            line = raw_line.decode(
                "utf-8",
                errors="ignore"
            ).strip()


            if not line:
                continue


            result = parse_line(line)


            # Ignore startup messages such as:
            #
            # STM32 BOOST CONTROLLER READY

            if result is None:
                continue


            (
                sample_count,
                vout_mv,
                error_mv,
                duty_permille,
                ovp,
                fb_fault
            ) = result


            # ------------------------------------------------
            # Unit conversion
            # ------------------------------------------------

            if time_origin_count is None:
                time_origin_count = sample_count

            time_sec = (
                (sample_count - time_origin_count) /
                CONTROL_FREQ_HZ
            )

            vout_v = (
                vout_mv /
                1000.0
            )

            duty_percent = (
                duty_permille /
                10.0
            )


            # ------------------------------------------------
            # Save data
            # ------------------------------------------------

            time_data.append(
                time_sec
            )

            vout_data.append(
                vout_v
            )

            duty_data.append(
                duty_percent
            )


            latest_vout = vout_v
            latest_duty = duty_percent
            latest_error = error_mv
            latest_ovp = ovp
            latest_fb_fault = fb_fault


        except Exception as e:

            print(
                "Serial parse error:",
                e
            )


# ============================================================
# GUI update
# ============================================================

def update_gui():

    read_serial()


    # --------------------------------------------------------
    # Update numeric values
    # --------------------------------------------------------

    vout_label.config(
        text=f"Vout: {latest_vout:.3f} V"
    )


    duty_label.config(
        text=f"Duty: {latest_duty:.1f} %"
    )


    error_label.config(
        text=f"Error: {latest_error} mV"
    )


    if latest_ovp:

        fault_label.config(
            text="FAULT: OVP",
            fg="red"
        )

    elif latest_fb_fault:

        fault_label.config(
            text="FAULT: FEEDBACK",
            fg="red"
        )

    else:

        fault_label.config(
            text="FAULT: NONE",
            fg="green"
        )


    # --------------------------------------------------------
    # Draw Vout
    # --------------------------------------------------------

    draw_plot(
        voltage_canvas,
        time_data,
        vout_data,
        y_min=4.5,
        y_max=8.0,
        title="Boost Converter Output Voltage",
        unit="Vout (V)",
        reference=7.0,
        y_tick_step=0.25
    )


    # --------------------------------------------------------
    # Draw Duty
    # --------------------------------------------------------

    draw_plot(
        duty_canvas,
        time_data,
        duty_data,
        y_min=20.0,
        y_max=70.0,
        title="PWM Duty Cycle",
        unit="Duty (%)",
        y_tick_step=10.0
    )


    # Update GUI every 20 ms
    root.after(
        20,
        update_gui
    )


# ============================================================
# Close handler
# ============================================================

def close_program():

    if ser.is_open:
        ser.close()

    root.destroy()


root.protocol(
    "WM_DELETE_WINDOW",
    close_program
)


# ============================================================
# Start
# ============================================================

update_gui()

root.mainloop()