import asyncio
import json
import math
import random
import time
from dataclasses import dataclass

import paho.mqtt.client as mqtt


# ============================================================
# CONFIGURATION
# ============================================================

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_TOPIC = "vehicle/can"
MQTT_CLIENT_ID = "vhms-can-simulator"

# CAN update rate
UPDATE_INTERVAL = 1.5


# ============================================================
# CAN FRAME
# ============================================================

@dataclass
class CANFrame:

    can_id: int
    data: bytes

    @property
    def dlc(self):
        return len(self.data)

    def __str__(self):

        data_string = " ".join(
            f"{b:02X}"
            for b in self.data
        )

        return (
            f"CAN ID: 0x{self.can_id:03X} | "
            f"DLC: {self.dlc} | "
            f"DATA: {data_string}"
        )


# ============================================================
# VIRTUAL CAN BUS
# ============================================================

class VirtualCANBus:

    def __init__(self):

        self.queue = asyncio.Queue()

    async def send(self, frame):

        await self.queue.put(frame)

    async def receive(self):

        return await self.queue.get()


# ============================================================
# CAN ENCODING
# ============================================================

def encode_uint16(value):
    """
    Convert integer to 2-byte big-endian CAN data.
    """

    value = int(value)

    return bytes([
        (value >> 8) & 0xFF,
        value & 0xFF
    ])


def decode_uint16(data):
    """
    Convert 2-byte big-endian CAN data back to integer.
    """

    return (data[0] << 8) | data[1]


# ============================================================
# ECU BASE CLASS
# ============================================================

class ECU:

    def __init__(self, name, bus):

        self.name = name
        self.bus = bus

    async def transmit(
        self,
        can_id,
        value,
        scale
    ):

        raw_value = int(value * scale)

        data = encode_uint16(raw_value)

        frame = CANFrame(
            can_id=can_id,
            data=data
        )

        await self.bus.send(frame)

        print(
            f"[{self.name}] TX → "
            f"0x{can_id:03X} | "
            f"value={value:.2f}"
        )


# ============================================================
# STM32 ECU
# ============================================================

class STM32ECU(ECU):

    async def run(self):

        t = 0

        while True:

            # ------------------------------------------------
            # Engine Temperature
            # CAN ID: 0x100
            # Scale: ×10
            # ------------------------------------------------

            engine_temp = (
                78
                + 3 * math.sin(t)
                + random.uniform(-0.5, 0.5)
            )

            # ------------------------------------------------
            # Engine RPM
            # CAN ID: 0x101
            # Scale: ×1
            # ------------------------------------------------

            rpm = (
                1800
                + 400 * math.sin(t * 0.7)
                + random.uniform(-50, 50)
            )

            # ------------------------------------------------
            # Vibration
            # CAN ID: 0x102
            # Scale: ×100
            # ------------------------------------------------

            vibration = (
                0.25
                + 0.08 * abs(math.sin(t * 2))
                + random.uniform(-0.02, 0.02)
            )

            await self.transmit(
                0x100,
                engine_temp,
                10
            )

            await self.transmit(
                0x101,
                rpm,
                1
            )

            await self.transmit(
                0x102,
                vibration,
                100
            )

            t += 0.1

            await asyncio.sleep(
                UPDATE_INTERVAL
            )


# ============================================================
# ARDUINO ECU
# ============================================================

class ArduinoECU(ECU):

    async def run(self):

        t = 0

        while True:

            # ------------------------------------------------
            # Ambient Temperature
            # CAN ID: 0x103
            # Scale: ×10
            # ------------------------------------------------

            ambient_temp = (
                29
                + 2 * math.sin(t * 0.5)
                + random.uniform(-0.3, 0.3)
            )

            # ------------------------------------------------
            # Humidity
            # CAN ID: 0x104
            # Scale: ×10
            # ------------------------------------------------

            humidity = (
                60
                + 5 * math.sin(t * 0.3)
                + random.uniform(-1, 1)
            )

            # ------------------------------------------------
            # Vehicle Speed
            # CAN ID: 0x105
            # Scale: ×10
            # ------------------------------------------------

            speed = (
                40
                + 15 * math.sin(t * 0.8)
                + random.uniform(-1, 1)
            )

            speed = max(
                0,
                speed
            )

            await self.transmit(
                0x103,
                ambient_temp,
                10
            )

            await self.transmit(
                0x104,
                humidity,
                10
            )

            await self.transmit(
                0x105,
                speed,
                10
            )

            t += 0.1

            await asyncio.sleep(
                UPDATE_INTERVAL
            )


# ============================================================
# ESP32 ECU
# ============================================================

class ESP32ECU(ECU):

    async def run(self):

        t = 0

        while True:

            # ------------------------------------------------
            # Battery Voltage
            # CAN ID: 0x106
            # Scale: ×10
            # ------------------------------------------------

            voltage = (
                13.8
                + 0.3 * math.sin(t * 0.4)
                + random.uniform(-0.05, 0.05)
            )

            # ------------------------------------------------
            # Battery Current
            # CAN ID: 0x107
            # Scale: ×10
            # ------------------------------------------------

            current = (
                4.0
                + 1.5 * abs(math.sin(t))
                + random.uniform(-0.1, 0.1)
            )

            await self.transmit(
                0x106,
                voltage,
                10
            )

            await self.transmit(
                0x107,
                current,
                10
            )

            t += 0.1

            await asyncio.sleep(
                UPDATE_INTERVAL
            )


# ============================================================
# CAN DECODER
# ============================================================

class CANDecoder:

    def __init__(self):

        self.values = {

            "temperature": 0.0,

            "vibration": 0.0,

            "batteryVoltage": 0.0,

            "engineTemp": 0.0,

            "rpm": 0,

            "batteryCurrent": 0.0,

            "speed": 0.0,

            "humidity": 0.0
        }

        self.can_messages = {}

    def decode(self, frame):

        raw = decode_uint16(
            frame.data
        )

        can_id = frame.can_id

        # ====================================================
        # STM32
        # ====================================================

        if can_id == 0x100:

            value = raw / 10

            self.values[
                "engineTemp"
            ] = value

            signal = "Engine Temperature"

            unit = "°C"

        elif can_id == 0x101:

            value = raw

            self.values[
                "rpm"
            ] = value

            signal = "Engine RPM"

            unit = "RPM"

        elif can_id == 0x102:

            value = raw / 100

            self.values[
                "vibration"
            ] = value

            signal = "Vibration"

            unit = "g"

        # ====================================================
        # ARDUINO
        # ====================================================

        elif can_id == 0x103:

            value = raw / 10

            self.values[
                "temperature"
            ] = value

            signal = "Ambient Temperature"

            unit = "°C"

        elif can_id == 0x104:

            value = raw / 10

            self.values[
                "humidity"
            ] = value

            signal = "Humidity"

            unit = "%"

        elif can_id == 0x105:

            value = raw / 10

            self.values[
                "speed"
            ] = value

            signal = "Vehicle Speed"

            unit = "km/h"

        # ====================================================
        # ESP32
        # ====================================================

        elif can_id == 0x106:

            value = raw / 10

            self.values[
                "batteryVoltage"
            ] = value

            signal = "Battery Voltage"

            unit = "V"

        elif can_id == 0x107:

            value = raw / 10

            self.values[
                "batteryCurrent"
            ] = value

            signal = "Battery Current"

            unit = "A"

        else:

            return None

        # ====================================================
        # STORE CAN FRAME INFORMATION
        # ====================================================

        self.can_messages[
            f"0x{can_id:03X}"
        ] = {

            "signal": signal,

            "value": value,

            "unit": unit,

            "data":
                frame.data.hex(
                    " "
                ).upper()
        }

        return {

            "can_id":
                f"0x{can_id:03X}",

            "signal":
                signal,

            "value":
                value,

            "unit":
                unit
        }


# ============================================================
# MQTT PUBLISHER
# ============================================================

class MQTTPublisher:

    def __init__(self):

        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=MQTT_CLIENT_ID
        )

        # Automatic reconnect
        self.client.reconnect_delay_set(
            min_delay=1,
            max_delay=30
        )

        self.connected = False

        self.client.on_connect = (
            self.on_connect
        )

        self.client.on_disconnect = (
            self.on_disconnect
        )

    def on_connect(
        self,
        client,
        userdata,
        flags,
        reason_code,
        properties
    ):

        if reason_code == 0:

            self.connected = True

            print()
            print(
                f"[MQTT] Connected to "
                f"{MQTT_BROKER}:{MQTT_PORT}"
            )

            print(
                f"[MQTT] Topic: "
                f"{MQTT_TOPIC}"
            )

        else:

            self.connected = False

            print(
                f"[MQTT] Connection failed: "
                f"{reason_code}"
            )

    def on_disconnect(
        self,
        client,
        userdata,
        disconnect_flags,
        reason_code,
        properties
    ):

        self.connected = False

        print(
            f"[MQTT] Disconnected: "
            f"{reason_code}"
        )

    def connect(self):

        print()
        print(
            "Connecting to MQTT broker..."
        )

        print(
            f"{MQTT_BROKER}:{MQTT_PORT}"
        )

        self.client.connect(
            MQTT_BROKER,
            MQTT_PORT,
            keepalive=60
        )

        # Start MQTT network loop
        self.client.loop_start()

        # Wait for connection
        deadline = (
            time.time() + 5
        )

        while (
            not self.connected
            and time.time() < deadline
        ):

            time.sleep(
                0.05
            )

        if not self.connected:

            raise RuntimeError(
                "Could not connect to MQTT broker"
            )

        print(
            "✓ MQTT connection established"
        )

        print()

    def publish_frame(
        self,
        frame
    ):

        payload = {

            "canId":
                f"0x{frame.can_id:03X}",

            "dlc":
                frame.dlc,

            "data":
                frame.data.hex(
                    " "
                ).upper(),

            "timestamp":
                time.time()
        }

        try:

            result = self.client.publish(

                MQTT_TOPIC,

                json.dumps(
                    payload
                ),

                qos=1,

                retain=False
            )

            if (
                result.rc
                != mqtt.MQTT_ERR_SUCCESS
            ):

                print(
                    f"[MQTT] Publish failed: "
                    f"{result.rc}"
                )

                return False

            print(
                f"[MQTT TX] "
                f"{payload['canId']} | "
                f"DLC={payload['dlc']} | "
                f"DATA={payload['data']}"
            )

            return True

        except Exception as e:

            print(
                "[MQTT] Publish error:",
                e
            )

            return False

    def disconnect(self):

        try:

            self.client.loop_stop()

            self.client.disconnect()

        except Exception:

            pass


# ============================================================
# CAN RECEIVER
# ============================================================

async def can_receiver(
    bus,
    decoder,
    mqtt_publisher
):

    while True:

        frame = await bus.receive()

        result = decoder.decode(
            frame
        )

        if result:

            print(
                f"[CAN RX] "
                f"{result['can_id']} | "
                f"{result['signal']} = "
                f"{result['value']:.2f} "
                f"{result['unit']}"
            )

            # Publish raw CAN frame to MQTT
            mqtt_publisher.publish_frame(
                frame
            )


# ============================================================
# MAIN
# ============================================================

async def main():

    print()

    print(
        "=============================================="
    )

    print(
        "       VEHICLE CAN NETWORK SIMULATOR"
    )

    print(
        "=============================================="
    )

    print()

    # --------------------------------------------------------
    # Create virtual CAN bus
    # --------------------------------------------------------

    bus = VirtualCANBus()

    # --------------------------------------------------------
    # Create decoder
    # --------------------------------------------------------

    decoder = CANDecoder()

    # --------------------------------------------------------
    # Create MQTT publisher
    # --------------------------------------------------------

    mqtt_publisher = MQTTPublisher()

    mqtt_publisher.connect()

    # --------------------------------------------------------
    # Create ECUs
    # --------------------------------------------------------

    stm32 = STM32ECU(
        "STM32",
        bus
    )

    arduino = ArduinoECU(
        "Arduino",
        bus
    )

    esp32 = ESP32ECU(
        "ESP32",
        bus
    )

    print(
        "Virtual CAN bus started"
    )

    print()

    # --------------------------------------------------------
    # Run everything simultaneously
    # --------------------------------------------------------

    try:

        await asyncio.gather(

            stm32.run(),

            arduino.run(),

            esp32.run(),

            can_receiver(
                bus,
                decoder,
                mqtt_publisher
            )
        )

    finally:

        mqtt_publisher.disconnect()


# ============================================================
# START
# ============================================================

if __name__ == "__main__":

    try:

        asyncio.run(
            main()
        )

    except KeyboardInterrupt:

        print()

        print(
            "Simulator stopped."
        )