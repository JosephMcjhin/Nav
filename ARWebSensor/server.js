const express = require('express');
const https = require('https');
const http = require('http');
const WebSocket = require('ws');
const dgram = require('dgram');
const selfsigned = require('selfsigned');
const path = require('path');
const os = require('os');

// --- 1. Generate Self-Signed Certs for HTTPS (Required for WebXR and Mobile sensors) ---
console.log('Generating self-signed certificate for local HTTPS...');
const attrs = [{ name: 'commonName', value: 'localhost' }];
const pems = selfsigned.generate(attrs, { days: 365, keySize: 2048 });

const app = express();
app.use(express.static(path.join(__dirname, 'public')));

// Create HTTPS Server
const server = https.createServer({
    key: pems.private,
    cert: pems.cert
}, app);

// Create HTTP Server (for local testing/fallback)
const httpServer = http.createServer(app);

// --- 2. Setup WebSocket Server to receive Mobile data ---
const wss = new WebSocket.Server({ server: server });
const wsHttp = new WebSocket.Server({ server: httpServer });

// --- 3. Setup UDP Client to send Data to Unreal Engine ---
const udpClient = dgram.createSocket('udp4');
const UE_PORT = 9003;
const UE_HOST = '127.0.0.1';

// Function to handle WebSocket connections
function setupWS(ws) {
    console.log('Mobile device connected via WebSocket!');

    ws.on('message', (message) => {
        try {
            const data = JSON.parse(message);
            // Example data from client: { x: 1.0, y: 0.0, z: -2.0, yaw: 45.0 }
            if (data.x !== undefined) {
                // Formatting payload as "X,Y,Z,Yaw" to match UE C++ Parsing
                // Unreal uses Z as up, Y as right, X as forward. We map WebXR coordinates mapping to UE:
                // WebXR: X right, Y up, Z backward (-Z forward)
                // UE: X forward, Y right, Z up
                // We will let the client HTML do the coordinate mapping, so server just forwards it directly.
                const udpPayload = `${data.x},${data.y},${data.z},${data.yaw}`;

                // Send UDP to Unreal
                udpClient.send(udpPayload, UE_PORT, UE_HOST, (err) => {
                    if (err) console.error('UDP Send Error:', err);
                });
            }
        } catch (e) {
            console.error('JSON Parse Error:', e);
        }
    });

    ws.on('close', () => console.log('Mobile device disconnected.'));
}

wss.on('connection', setupWS);
wsHttp.on('connection', setupWS);

// --- 4. Get Local IP Address to show the user ---
function getLocalIP() {
    const interfaces = os.networkInterfaces();
    for (const name of Object.keys(interfaces)) {
        for (const iface of interfaces[name]) {
            if (iface.family === 'IPv4' && !iface.internal) {
                return iface.address;
            }
        }
    }
    return '127.0.0.1';
}

const LOCAL_IP = getLocalIP();
const HTTPS_PORT = 8080;
const HTTP_PORT = 8081;

server.listen(HTTPS_PORT, '0.0.0.0', () => {
    console.log(`\n=================================================`);
    console.log(`🚀 RELAY SERVER RUNNING!`);
    console.log(`=================================================`);
    console.log(`Please connect your phone to the same WiFi.`);
    console.log(`Then, open this URL in your mobile browser (Safari/Chrome):`);
    console.log(`\n    👉 https://${LOCAL_IP}:${HTTPS_PORT} 👈\n`);
    console.log(`(Note: Your browser will warn you about the connection being unsafe because we generated a self-signed certificate.`);
    console.log(`Please click "Advanced" -> "Proceed anyway" to allow access to the sensors)`);
    console.log(`=================================================\n`);
});

httpServer.listen(HTTP_PORT, '0.0.0.0', () => {
    // HTTP fallback mostly for local PC testing. Mobile requires HTTPS for sensors.
});
