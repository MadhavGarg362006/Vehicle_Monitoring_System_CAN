import matplotlib.pyplot as plt

# ============================================================
# VEHICLE HEALTH MONITORING SYSTEM - PYTHON SIMULATION
# ============================================================

# Test readings
# Each list contains 3 readings taken at different test times.

time = [1, 2, 3]   # Test reading number / time

temperature = [28.5, 29.2, 30.1]       # °C
vibration = [0.25, 0.31, 0.42]         # RMS / arbitrary unit
battery_voltage = [12.6, 12.4, 12.2]   # V
engine_temperature = [72, 76, 82]      # °C
engine_rpm = [1200, 1800, 2500]        # RPM
battery_current = [2.1, 3.4, 4.8]      # A
vehicle_speed = [0, 15, 30]             # km/h
humidity = [52, 54, 57]                 # %

# ============================================================
# PRINT THE READINGS
# ============================================================

print("\nVEHICLE HEALTH MONITORING TEST DATA")
print("------------------------------------")

for i in range(len(time)):
    print(f"\nReading {i + 1}")
    print(f"Temperature        : {temperature[i]} °C")
    print(f"Vibration          : {vibration[i]}")
    print(f"Battery Voltage    : {battery_voltage[i]} V")
    print(f"Engine Temperature : {engine_temperature[i]} °C")
    print(f"Engine RPM         : {engine_rpm[i]} RPM")
    print(f"Battery Current    : {battery_current[i]} A")
    print(f"Vehicle Speed      : {vehicle_speed[i]} km/h")
    print(f"Humidity           : {humidity[i]} %")

# ============================================================
# CREATE GRAPH
# ============================================================

fig, axes = plt.subplots(4, 2, figsize=(14, 12))

# Temperature
axes[0, 0].plot(time, temperature, marker='o')
axes[0, 0].set_title("Ambient Temperature")
axes[0, 0].set_xlabel("Test Reading")
axes[0, 0].set_ylabel("Temperature (°C)")
axes[0, 0].grid(True)

# Vibration
axes[0, 1].plot(time, vibration, marker='o')
axes[0, 1].set_title("Vibration")
axes[0, 1].set_xlabel("Test Reading")
axes[0, 1].set_ylabel("Vibration")
axes[0, 1].grid(True)

# Battery Voltage
axes[1, 0].plot(time, battery_voltage, marker='o')
axes[1, 0].set_title("Battery Voltage")
axes[1, 0].set_xlabel("Test Reading")
axes[1, 0].set_ylabel("Voltage (V)")
axes[1, 0].grid(True)

# Engine Temperature
axes[1, 1].plot(time, engine_temperature, marker='o')
axes[1, 1].set_title("Engine Temperature")
axes[1, 1].set_xlabel("Test Reading")
axes[1, 1].set_ylabel("Temperature (°C)")
axes[1, 1].grid(True)

# Engine RPM
axes[2, 0].plot(time, engine_rpm, marker='o')
axes[2, 0].set_title("Engine RPM")
axes[2, 0].set_xlabel("Test Reading")
axes[2, 0].set_ylabel("RPM")
axes[2, 0].grid(True)

# Battery Current
axes[2, 1].plot(time, battery_current, marker='o')
axes[2, 1].set_title("Battery Current")
axes[2, 1].set_xlabel("Test Reading")
axes[2, 1].set_ylabel("Current (A)")
axes[2, 1].grid(True)

# Vehicle Speed
axes[3, 0].plot(time, vehicle_speed, marker='o')
axes[3, 0].set_title("Vehicle Speed")
axes[3, 0].set_xlabel("Test Reading")
axes[3, 0].set_ylabel("Speed (km/h)")
axes[3, 0].grid(True)

# Humidity
axes[3, 1].plot(time, humidity, marker='o')
axes[3, 1].set_title("Humidity")
axes[3, 1].set_xlabel("Test Reading")
axes[3, 1].set_ylabel("Humidity (%)")
axes[3, 1].grid(True)

# Overall title
fig.suptitle(
    "Vehicle Health Monitoring System - Sensor Data",
    fontsize=16,
    fontweight="bold"
)

plt.tight_layout()
plt.show()