import requests
import time

# ThingSpeak API credentials
MASTER_API_KEY = "YOUR_MASTER_API_KEY"  # Used to fetch all channels
THINGSPEAK_URL = "https://api.thingspeak.com"

# Vital thresholds
TEMP_THRESHOLD = 38.0   # High fever
HR_LOW_THRESHOLD = 50   # Low heart rate
HR_HIGH_THRESHOLD = 120 # High heart rate
SPO2_THRESHOLD = 90     # Low oxygen levels

# Function to get all patient channels dynamically
def get_patient_channels():
    url = f"{THINGSPEAK_URL}/channels.json?api_key={MASTER_API_KEY}"
    response = requests.get(url)
    
    if response.status_code == 200:
        channels = response.json()
        patient_channels = {}
        for channel in channels:
            if 'name' in channel and 'Patient_' in channel['name']:
                patient_id = channel['name'].split('_')[1]
                patient_channels[patient_id] = {
                    "channel_id": channel["id"],
                    "read_api_key": channel["api_keys"][0]["api_key"]
                }
        return patient_channels
    else:
        print("Failed to fetch patient channels.")
        return {}

# Function to fetch latest patient data
def get_latest_vitals(channel_id, read_api_key):
    url = f"{THINGSPEAK_URL}/channels/{channel_id}/feeds.json?api_key={read_api_key}&results=1"
    response = requests.get(url)
    
    if response.status_code == 200:
        data = response.json()
        if "feeds" in data and len(data["feeds"]) > 0:
            latest_feed = data["feeds"][0]
            return {
                "temperature": float(latest_feed.get("field1", 0)),
                "humidity": float(latest_feed.get("field2", 0)),
                "heart_rate": float(latest_feed.get("field3", 0)),
                "spo2": float(latest_feed.get("field4", 0)),
            }
    
    return None

# Function to check for abnormalities
def check_abnormalities(patient_id, vitals):
    alerts = []
    
    if vitals["temperature"] > TEMP_THRESHOLD:
        alerts.append("High temperature detected!")
    if vitals["heart_rate"] < HR_LOW_THRESHOLD:
        alerts.append("Low heart rate detected!")
    if vitals["heart_rate"] > HR_HIGH_THRESHOLD:
        alerts.append("High heart rate detected!")
    if vitals["spo2"] < SPO2_THRESHOLD:
        alerts.append("Low SpO2 level detected!")
    
    if alerts:
        print(f"\nALERT for Patient {patient_id}:")
        for alert in alerts:
            print(f"  - {alert}")
    else:
        print(f"Patient {patient_id} vitals are normal.")

# Main loop
while True:
    print("Fetching patient list...")
    patient_channels = get_patient_channels()
    
    for patient_id, details in patient_channels.items():
        vitals = get_latest_vitals(details["channel_id"], details["read_api_key"])
        if vitals:
            print(f"\nPatient {patient_id} Vitals:")
            print(f"  Temperature: {vitals['temperature']}°C")
            print(f"  Heart Rate: {vitals['heart_rate']} BPM")
            print(f"  SpO2: {vitals['spo2']}%")
            check_abnormalities(patient_id, vitals)
        else:
            print(f"Failed to fetch data for Patient {patient_id}.")
    
    print("\nWaiting for the next update...\n")
    time.sleep(15)  # Wait 15 seconds before next update
