// server.js - Express server to handle ThingSpeak data and serve the frontend
const express = require('express');
const http = require('http');
const socketIo = require('socket.io');
const mqtt = require('mqtt');
const cors = require('cors');
const mongoose = require('mongoose');
require('dotenv').config();

const app = express();
const server = http.createServer(app);
const io = socketIo(server, {
  cors: {
    origin: '*',
    methods: ['GET', 'POST']
  }
});

// Middleware
app.use(cors());
app.use(express.json());
app.use(express.static('public'));

// MongoDB connection
mongoose.connect(process.env.MONGODB_URI || 'mongodb://localhost:27017/patient_monitoring', {
  useNewUrlParser: true,
  useUnifiedTopology: true
});

// Database models
const patientSchema = new mongoose.Schema({
  patientId: { type: String, required: true, unique: true },
  name: { type: String, required: true },
  age: Number,
  gender: String,
  condition: String,
  thingspeakChannelId: String,
  thingspeakReadApiKey: String,
  vitalThresholds: {
    heartRate: { min: Number, max: Number },
    temperature: { min: Number, max: Number },
    bloodPressure: { min: Number, max: Number },
    oxygenSaturation: { min: Number, max: Number }
  },
  lastUpdated: { type: Date, default: Date.now }
});

const readingSchema = new mongoose.Schema({
  patientId: { type: String, required: true },
  timestamp: { type: Date, default: Date.now },
  heartRate: Number,
  temperature: Number,
  bloodPressureSystolic: Number,
  bloodPressureDiastolic: Number,
  oxygenSaturation: Number,
  isAlert: { type: Boolean, default: false }
});

const Patient = mongoose.model('Patient', patientSchema);
const Reading = mongoose.model('Reading', readingSchema);

// API routes
app.get('/api/patients', async (req, res) => {
  try {
    const patients = await Patient.find();
    res.json(patients);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

app.get('/api/patients/:patientId', async (req, res) => {
  try {
    const patient = await Patient.findOne({ patientId: req.params.patientId });
    if (!patient) {
      return res.status(404).json({ error: 'Patient not found' });
    }
    res.json(patient);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

app.get('/api/readings/:patientId', async (req, res) => {
  try {
    const readings = await Reading.find({ 
      patientId: req.params.patientId 
    }).sort({ timestamp: -1 }).limit(100);
    
    res.json(readings);
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

// ThingSpeak MQTT connection
const mqttClient = mqtt.connect('mqtt://mqtt3.thingspeak.com', {
  username: process.env.THINGSPEAK_MQTT_USERNAME || 'your_mqtt_username',
  password: process.env.THINGSPEAK_MQTT_PASSWORD || 'your_mqtt_password'
});

mqttClient.on('connect', () => {
  console.log('Connected to ThingSpeak MQTT broker');
  
  // Subscribe to channels (you'll need to adjust these topics based on your ThingSpeak setup)
  mqttClient.subscribe('channels/+/subscribe/fields/+');
});

mqttClient.on('message', async (topic, message) => {
  try {
    // Parse the topic to extract channel and field
    const topicParts = topic.split('/');
    const channelId = topicParts[1];
    const fieldNumber = topicParts[3];
    
    // Find which patient this belongs to
    const patient = await Patient.findOne({ thingspeakChannelId: channelId });
    if (!patient) return;
    
    // Process the data based on field number and save to database
    const messageValue = parseFloat(message.toString());
    let updatedReading = {};
    let isAlert = false;
    
    // Each field represents different sensor data
    // This mapping depends on how you've set up your ThingSpeak channels
    switch(fieldNumber) {
      case 'field1':
        updatedReading.heartRate = messageValue;
        if (messageValue < patient.vitalThresholds.heartRate.min || 
            messageValue > patient.vitalThresholds.heartRate.max) {
          isAlert = true;
        }
        break;
      case 'field2':
        updatedReading.temperature = messageValue;
        if (messageValue < patient.vitalThresholds.temperature.min || 
            messageValue > patient.vitalThresholds.temperature.max) {
          isAlert = true;
        }
        break;
      case 'field3':
        updatedReading.bloodPressureSystolic = messageValue;
        if (messageValue < patient.vitalThresholds.bloodPressure.min || 
            messageValue > patient.vitalThresholds.bloodPressure.max) {
          isAlert = true;
        }
        break;
      case 'field4':
        updatedReading.bloodPressureDiastolic = messageValue;
        break;
      case 'field5':
        updatedReading.oxygenSaturation = messageValue;
        if (messageValue < patient.vitalThresholds.oxygenSaturation.min || 
            messageValue > patient.vitalThresholds.oxygenSaturation.max) {
          isAlert = true;
        }
        break;
    }
    
    // Update the latest reading for this patient
    let reading = await Reading.findOne({ 
      patientId: patient.patientId,
      timestamp: { 
        $gte: new Date(Date.now() - 60000) // Get reading from last minute
      }
    });
    
    if (reading) {
      // Update existing reading
      Object.assign(reading, updatedReading);
      if (isAlert) reading.isAlert = true;
      await reading.save();
    } else {
      // Create new reading
      reading = new Reading({
        patientId: patient.patientId,
        ...updatedReading,
        isAlert
      });
      await reading.save();
    }
    
    // Emit the data to connected clients
    io.emit('patientUpdate', {
      patientId: patient.patientId,
      reading: {
        ...updatedReading,
        timestamp: new Date(),
        isAlert
      }
    });
    
    // If there's an alert, emit a special alert event
    if (isAlert) {
      io.emit('patientAlert', {
        patientId: patient.patientId,
        name: patient.name,
        reading: {
          ...updatedReading,
          timestamp: new Date()
        }
      });
    }
  } catch (error) {
    console.error('Error processing MQTT message:', error);
  }
});

// Socket.io for real-time updates
io.on('connection', socket => {
  console.log('New client connected');
  
  socket.on('disconnect', () => {
    console.log('Client disconnected');
  });
});

// Start server
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`Server running on port ${PORT}`);
});

// For demo/testing, create sample patients if none exist
async function initializeSampleData() {
  const count = await Patient.countDocuments();
  if (count === 0) {
    const samplePatients = [
      {
        patientId: 'P001',
        name: 'John Doe',
        age: 65,
        gender: 'Male',
        condition: 'Hypertension',
        thingspeakChannelId: '1234567',
        thingspeakReadApiKey: 'SAMPLE_API_KEY_1',
        vitalThresholds: {
          heartRate: { min: 60, max: 100 },
          temperature: { min: 36.5, max: 37.5 },
          bloodPressure: { min: 90, max: 140 },
          oxygenSaturation: { min: 95, max: 100 }
        }
      },
      {
        patientId: 'P002',
        name: 'Jane Smith',
        age: 42,
        gender: 'Female',
        condition: 'Diabetes',
        thingspeakChannelId: '7654321',
        thingspeakReadApiKey: 'SAMPLE_API_KEY_2',
        vitalThresholds: {
          heartRate: { min: 60, max: 100 },
          temperature: { min: 36.5, max: 37.5 },
          bloodPressure: { min: 90, max: 140 },
          oxygenSaturation: { min: 95, max: 100 }
        }
      },
      {
        patientId: 'P003',
        name: 'Robert Johnson',
        age: 78,
        gender: 'Male',
        condition: 'Heart Disease',
        thingspeakChannelId: '2468135',
        thingspeakReadApiKey: 'SAMPLE_API_KEY_3',
        vitalThresholds: {
          heartRate: { min: 55, max: 90 },
          temperature: { min: 36.5, max: 37.5 },
          bloodPressure: { min: 100, max: 150 },
          oxygenSaturation: { min: 92, max: 100 }
        }
      }
    ];

    await Patient.insertMany(samplePatients);
    console.log('Sample patients created');
  }
}

// Call initialization function
mongoose.connection.once('open', () => {
  console.log('MongoDB connected');
  initializeSampleData();
});