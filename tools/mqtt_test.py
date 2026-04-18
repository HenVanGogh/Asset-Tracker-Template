#!/usr/bin/env python3
"""
MQTT Test Script for Thingy:91 X Asset Tracker
Tests UART passthrough commands and monitors responses.
"""

import json
import time
import argparse
import paho.mqtt.client as mqtt
from datetime import datetime


class AssetTrackerTester:
    def __init__(self, broker_host, broker_port, username, password, device_id="gateway_XXXX"):
        self.broker_host = broker_host
        self.broker_port = broker_port
        self.username = username
        self.password = password
        self.device_id = device_id
        
        self.command_topic = f"gateway/{device_id}/command"
        self.data_topic = f"gateway/{device_id}/data"
        
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"test-client-{int(time.time())}")
        self.client.username_pw_set(username, password)
        
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        
        self.responses = []
        self.connected = False
        
    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            print(f"✓ Connected to MQTT broker {self.broker_host}:{self.broker_port}")
            self.connected = True
            client.subscribe(self.data_topic)
            print(f"✓ Subscribed to: {self.data_topic}")
        else:
            print(f"✗ Connection failed with code: {reason_code}")
            
    def _on_disconnect(self, client, userdata, disconnect_flags, reason_code, properties):
        print(f"Disconnected from broker (rc={reason_code})")
        self.connected = False
        
    def _on_message(self, client, userdata, msg):
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        try:
            payload = json.loads(msg.payload.decode())
            self.responses.append(payload)
            
            msg_type = payload.get("type", payload.get("status", "unknown"))
            print(f"\n[{timestamp}] Received ({msg_type}):")
            print(json.dumps(payload, indent=2))
        except json.JSONDecodeError:
            print(f"\n[{timestamp}] Raw message: {msg.payload.decode()}")
            
    def connect(self, timeout=10):
        print(f"Connecting to {self.broker_host}:{self.broker_port}...")
        self.client.connect(self.broker_host, self.broker_port, 60)
        self.client.loop_start()
        
        start = time.time()
        while not self.connected and (time.time() - start) < timeout:
            time.sleep(0.1)
            
        return self.connected
        
    def disconnect(self):
        self.client.loop_stop()
        self.client.disconnect()
        
    def send_uart_command(self, command):
        """Send a UART passthrough command."""
        payload = {
            "type": "uart_passthrough",
            "command": command
        }
        self._publish(payload)
        
    def send_command(self, command, args=None):
        """Send a standard command with optional args."""
        payload = {"command": command}
        if args:
            payload["args"] = args
        self._publish(payload)
        
    def send_nus_command(self, nus_cmd, sensor_addr=None):
        """
        Send a NUS command to a connected sensor via UART passthrough.
        
        NUS commands are sent using the format: send <addr> <command>
        If no sensor_addr is provided, sends to the first connected sensor.
        
        Safe commands (won't cause connection issues):
        - STATUS: Get device status
        - SENSORS: Get sensor readings
        - CONFIG: Get current configuration
        - HELP: Display help
        
        Configuration commands (use with caution):
        - NAME:<name>: Change device name
        - TX_PWR:<dbm>: Set TX power
        - ADV_INT:<min>,<max>: Set advertising interval
        - SENSOR:<name>,<0/1>: Enable/disable sensor
        - UPDATE:<ms>: Set update interval
        """
        # For NUS commands, we need to know which sensor to send to
        # If no address provided, just send the NUS command directly
        # The gateway will forward it to the first connected sensor
        if sensor_addr:
            uart_cmd = f"send {sensor_addr} {nus_cmd}"
        else:
            # Try to send to any connected sensor - gateway should handle this
            uart_cmd = f"send_all {nus_cmd}"
        
        self.send_uart_command(uart_cmd)
        
    def _publish(self, payload):
        msg = json.dumps(payload)
        result = self.client.publish(self.command_topic, msg, qos=1)
        print(f"\n→ Sent to {self.command_topic}:")
        print(json.dumps(payload, indent=2))
        return result
        
    def wait_for_responses(self, timeout=5, expected_count=1):
        """Wait for responses to arrive."""
        start = time.time()
        initial_count = len(self.responses)
        
        while (time.time() - start) < timeout:
            if len(self.responses) >= initial_count + expected_count:
                break
            time.sleep(0.1)
            
        new_responses = self.responses[initial_count:]
        return new_responses
        
    def clear_responses(self):
        self.responses = []


def run_test_suite(tester):
    """Run a comprehensive test suite with proper content validation."""
    print("\n" + "="*60)
    print("ASSET TRACKER MQTT TEST SUITE")
    print("="*60)
    
    # Test definitions: (name, test_fn, validator, timeout, is_nus)
    # validator: can be a list of (field, pattern) tuples OR a function(payload) returning bool
    def validate_sensor_report(payload):
        """Validate the specific structure of the sensor report."""
        try:
            # Check top-level fields
            if payload.get("type") != "uart_sensor":
                return False
            if payload.get("device_id") != self.device_id:
                return False
            if not isinstance(payload.get("sequence"), int):
                return False
            if not isinstance(payload.get("timestamp"), int):
                return False
                
            # Check data object
            data = payload.get("data", {})
            required_data = ["temperature", "humidity", "probe_name", "probe_battery", "sensor_timestamp"]
            if not all(k in data for k in required_data):
                return False
                
            # Check quality object
            quality = payload.get("quality", {})
            required_quality = ["temperature_valid", "humidity_valid", "battery_valid", "probe_name_valid"]
            if not all(k in quality for k in required_quality):
                return False
                
            return True
        except Exception:
            return False

    tests = [
        # Sensor Report Test - FIRST
        # This blocks for up to 12 minutes waiting for the periodic report.
        # Once received, we know the device is online and listening.
        (
            "Sensor Report Validation",
            lambda: print("  (Waiting for periodic report... don't interrupt!)"),
            validate_sensor_report,
            720,  # 12 minutes timeout
            False
        ),
        # Gateway tests - Run immediately after report received
        (
            "Get Device Status",
            lambda: tester.send_command("get_status"),
            [("status", "online")],
            5,
            False
        ),
        (
            "Gateway List Command",
            lambda: tester.send_uart_command("list"),
            [("data", "SENSORS_LIST")],
            10,
            False
        ),
        (
            "Gateway Status Command",
            lambda: tester.send_uart_command("status"),
            [("data", "STATUS:UP=")],
            5,
            False
        ),
        # NUS tests
        (
            "NUS STATUS",
            lambda: tester.send_nus_command("STATUS"),
            [("data", "RX_FROM_SENSOR:STATUS:")],
            15,
            True
        ),
        (
            "NUS SENSORS",
            lambda: tester.send_nus_command("SENSORS"),
            [("data", "RX_FROM_SENSOR:SENSORS:")],
            15,
            True
        ),
        (
            "NUS CONFIG",
            lambda: tester.send_nus_command("CONFIG"),
            [("data", "RX_FROM_SENSOR:CONFIG:")],
            15,
            True
        ),
        (
            "NUS HELP",
            lambda: tester.send_nus_command("HELP"),
            [("data", "RX_FROM_SENSOR:HELP:")],
            15,
            True
        ),
    ]
    
    results = []
    
    for name, test_fn, validator, timeout, is_nus in tests:
        print(f"\n--- Test: {name} ---")
        tester.clear_responses()
        
        test_fn()
        
        print(f"  (waiting up to {timeout}s for validation...)")
        
        # Smart wait: loop until validator passes or timeout
        start_time = time.time()
        validation_passed = False
        details = []
        
        while (time.time() - start_time) < timeout:
            responses = tester.responses.copy()
            details = []
            
            # Check validation
            if callable(validator):
                for resp in responses:
                    if validator(resp):
                        validation_passed = True
                        details.append("custom_validation_passed")
                        break
            else:
                matched_patterns = []
                for pattern_field, pattern_value in validator:
                    pattern_found = False
                    for resp in responses:
                        field_value = resp.get(pattern_field, "")
                        if isinstance(field_value, str) and pattern_value in field_value:
                            pattern_found = True
                            matched_patterns.append(f"{pattern_field}='{pattern_value}'")
                            break
                    if not pattern_found:
                        break
                else:
                    validation_passed = True
                    details = matched_patterns
            
            if validation_passed:
                break
                
            time.sleep(0.5)
        
        results.append((name, validation_passed, len(responses), details))
        
        if validation_passed:
            det_str = ', '.join(details)
            print(f"\nResult: ✓ PASS (validated: {det_str})")
        else:
            print(f"\nResult: ✗ FAIL (responses: {len(responses)})")
            if len(responses) > 0:
                print("Last response:")
                print(json.dumps(responses[-1], indent=2))
        
        # Additional delay to let things settle
        if is_nus:
            print("  (waiting 3s for BLE to settle...)")
            time.sleep(3)
        else:
            time.sleep(1)
    
    # Summary
    print("\n" + "="*60)
    print("TEST SUMMARY")
    print("="*60)
    
    gateway_tests = [r for r in results if not r[0].startswith("NUS ")]
    nus_tests = [r for r in results if r[0].startswith("NUS ")]
    
    print("\n  Gateway Tests:")
    for name, passed, count, patterns in gateway_tests:
        status = "✓" if passed else "✗"
        print(f"    {status} {name}: {count} responses")
    
    print("\n  NUS Command Tests:")
    for name, passed, count, patterns in nus_tests:
        status = "✓" if passed else "✗"
        print(f"    {status} {name}: {count} responses")
    
    passed_count = sum(1 for _, p, _, _ in results if p)
    print(f"\nTotal: {passed_count}/{len(results)} tests passed")
    
    return all(p for _, p, _, _ in results)


def interactive_mode(tester):
    """Run in interactive mode for manual testing."""
    print("\n" + "="*60)
    print("INTERACTIVE MODE")
    print("Commands: list, status, connect <addr>, disconnect <addr>")
    print("Special: get_status, get_location")
    print("Type 'quit' to exit")
    print("="*60)
    
    while True:
        try:
            cmd = input("\n> ").strip()
            if not cmd:
                continue
            if cmd.lower() == "quit":
                break
            elif cmd.lower() == "get_status":
                tester.send_command("get_status")
            elif cmd.lower() == "get_location":
                tester.send_command("get_location")
            else:
                tester.send_uart_command(cmd)
                
            tester.wait_for_responses(timeout=5)
        except KeyboardInterrupt:
            break
            

def main():
    parser = argparse.ArgumentParser(description="MQTT Test Script for Asset Tracker")
    parser.add_argument("--host", default="217.154.155.83", help="MQTT broker hostname")
    parser.add_argument("--port", type=int, default=1883, help="MQTT broker port")
    parser.add_argument("--user", default="mqttuser", help="MQTT username")
    parser.add_argument("--password", default="mqttuser", help="MQTT password")
    parser.add_argument("--device", default="gateway_XXXX", help="Device ID (e.g. gateway_A1F3)")
    parser.add_argument("--test", action="store_true", help="Run test suite")
    parser.add_argument("--interactive", "-i", action="store_true", help="Interactive mode")
    parser.add_argument("--command", "-c", help="Send a single UART command")
    
    args = parser.parse_args()
    
    tester = AssetTrackerTester(
        broker_host=args.host,
        broker_port=args.port,
        username=args.user,
        password=args.password,
        device_id=args.device
    )
    
    if not tester.connect():
        print("Failed to connect to broker")
        return 1
        
    try:
        if args.test:
            success = run_test_suite(tester)
            return 0 if success else 1
        elif args.interactive:
            interactive_mode(tester)
        elif args.command:
            tester.send_uart_command(args.command)
            tester.wait_for_responses(timeout=10)
        else:
            # Default: run basic test
            print("Waiting for messages (Ctrl+C to stop)...")
            tester.send_command("get_status")
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        tester.disconnect()
        
    return 0


if __name__ == "__main__":
    exit(main())
