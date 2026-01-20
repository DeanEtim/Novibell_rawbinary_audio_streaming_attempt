const char test_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>NoviBell Audio Streaming</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { 
            font-family: Arial, sans-serif; 
            margin: 0; 
            padding: 20px; 
            text-align: center;
            background-color: #f5f5f5;
            color: #333;
        }
        h1 { 
            color: #2c3e50; 
            margin-bottom: 15px;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            background-color: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }
        #waveform { 
            width: 100%; 
            height: 200px; 
            border: 1px solid #ddd; 
            margin: 20px 0;
            background-color: #fafafa;
            border-radius: 4px;
        }
        .status { 
            padding: 10px; 
            margin: 10px 0; 
            border-radius: 4px;
            font-weight: bold;
        }
        .connected { 
            background-color: #d4edda; 
            color: #155724; 
        }
        .disconnected { 
            background-color: #f8d7da; 
            color: #721c24; 
        }
        .connecting { 
            background-color: #fff3cd; 
            color: #856404; 
        }
        .controls { 
            margin: 20px 0;
            display: flex;
            justify-content: center;
            gap: 10px;
        }
        button { 
            padding: 10px 20px;
            background-color: #3498db;
            color: white;
            border: none;
            border-radius: 4px;
            cursor: pointer;
            font-weight: bold;
            transition: background-color 0.2s;
            min-width: 100px;
        }
        button:hover { 
            background-color: #2980b9; 
        }
        button:disabled { 
            background-color: #cccccc; 
            cursor: not-allowed; 
        }
        .audio-indicator {
            display: inline-block;
            width: 10px;
            height: 10px;
            border-radius: 50%;
            margin-right: 10px;
            background-color: #ccc;
        }
        .audio-active {
            background-color: #4CAF50;
            animation: pulse 1s infinite;
        }
        @keyframes pulse {
            0% { opacity: 1; }
            50% { opacity: 0.5; }
            100% { opacity: 1; }
        }
        .info {
            margin: 10px 0;
            padding: 10px;
            background-color: #e8f4f8;
            border-radius: 4px;
            font-size: 14px;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>NoviBell Audio Streaming Test</h1>
        <div id="connection-status" class="status disconnected">Disconnected</div>
        
        <div class="controls">
            <button id="playButton" disabled>
                <span class="audio-indicator" id="audioIndicator"></span>Play Audio
            </button>
            <button id="stopButton" disabled>Stop Audio</button>
        </div>
        
        <canvas id="waveform"></canvas>
    </div>
    
    <script>
        // Global variables
        let websocket;
        const canvas = document.getElementById('waveform');
        const ctx = canvas.getContext('2d');
        const statusDiv = document.getElementById('connection-status');
        const playButton = document.getElementById('playButton');
        const stopButton = document.getElementById('stopButton');
        const audioIndicator = document.getElementById('audioIndicator');
        
        // Audio processing variables
        let dataPoints = [];
        const maxDataPoints = 200; // Increased for higher sample rate
        let isPlaying = false;
        let audioContext;
        let audioBuffer = [];
        const maxAudioBufferSize = 65536; 
        let gainNode;
        let reconnectAttempts = 0;
        const maxReconnectAttempts = 5;
        
        // Set canvas dimensions
        function resizeCanvas() {
            canvas.width = canvas.offsetWidth;
            canvas.height = canvas.offsetHeight;
            drawWaveform();
        }
        
        // Initialize audio context
        function initAudio() {
            try {
                audioContext = new (window.AudioContext || window.webkitAudioContext)({
                    sampleRate: 44100 // Match ESP32 sample rate
                });
                
                gainNode = audioContext.createGain();
                gainNode.gain.value = 3.0; 
                gainNode.connect(audioContext.destination);
                
                return true;
            } catch (e) {
                console.error("Audio initialization failed:", e);
                statusDiv.textContent = "Audio initialization failed. Please try a different browser.";
                return false;
            }
        }
        
        // Initialize WebSocket connection
        function connectWebSocket() {
            if (reconnectAttempts >= maxReconnectAttempts) {
                statusDiv.textContent = "Connection failed after multiple attempts. Please reload the page.";
                statusDiv.className = "status disconnected";
                return;
            }
            
            statusDiv.textContent = "Connecting...";
            statusDiv.className = "status connecting";
            
            const protocol = window.location.protocol === 'https:' ? 'wss://' : 'ws://';
            websocket = new WebSocket(protocol + window.location.host + '/ws');
            
            // Set binary type for receiving binary data
            websocket.binaryType = 'arraybuffer';
            
            websocket.onopen = function(evt) {
                console.log('WebSocket connected');
                statusDiv.textContent = 'Connected';
                statusDiv.className = 'status connected';
                playButton.disabled = false;
                reconnectAttempts = 0;
            };
            
            websocket.onclose = function(evt) {
                console.log('WebSocket disconnected');
                statusDiv.textContent = 'Disconnected - Reconnecting...';
                statusDiv.className = 'status disconnected';
                playButton.disabled = true;
                stopButton.disabled = true;
                isPlaying = false;
                audioIndicator.classList.remove('audio-active');
                
                reconnectAttempts++;
                setTimeout(connectWebSocket, 2000);
            };
            
            websocket.onerror = function(evt) {
                console.error('WebSocket error:', evt);
            };
            
            websocket.onmessage = handleWebSocketMessage;
        }
        
        // Handle incoming WebSocket messages (binary data)
        function handleWebSocketMessage(evt) {
            try {
                // Handle binary data
                if (evt.data instanceof ArrayBuffer) {
                    // Convert ArrayBuffer to Int16Array (16-bit samples)
                    const samples = new Int16Array(evt.data);
                    
                    if (samples.length > 0) {
                        // Take every Nth sample for visualization to prevent overwhelming
                        const skipRate = Math.max(1, Math.floor(samples.length / 200));
                        for (let i = 0; i < samples.length; i += skipRate) {
                            dataPoints.push(samples[i]);
                        }
                        
                        // Limit visualization data points
                        if (dataPoints.length > maxDataPoints) {
                            dataPoints.splice(0, dataPoints.length - maxDataPoints);
                        }
                        
                        // Add to audio buffer if playing
                        if (isPlaying && audioContext) {
                            for (let i = 0; i < samples.length; i++) {
                                // Normalize 16-bit samples to -1.0 to 1.0 range for audio
                                audioBuffer.push(samples[i] / 32768.0);
                            }
                            
                            // Process audio when buffer reaches threshold
                            if (audioBuffer.length >= maxAudioBufferSize) {
                                processAudio();
                            }
                        }
                        
                        // Draw waveform
                        requestAnimationFrame(drawWaveform);
                    }
                } else {
                    // Handle text messages (commands)
                    console.log('Received text message:', evt.data);
                }
            } catch (e) {
                console.error('Error processing message:', e);
            }
        }
        
        // Process and play audio buffer
        function processAudio() {
            if (audioBuffer.length === 0 || !audioContext || !isPlaying) return;
            
            try {
                // Create an audio buffer
                const buffer = audioContext.createBuffer(1, audioBuffer.length, audioContext.sampleRate);
                const channel = buffer.getChannelData(0);
                
                // Copy data to the buffer
                for (let i = 0; i < audioBuffer.length; i++) {
                    channel[i] = audioBuffer[i];
                }
                
                // Create and connect the source
                const source = audioContext.createBufferSource();
                source.buffer = buffer;
                source.connect(gainNode);
                
                // Play the buffer
                source.start();
                
                // Clear the buffer for new data
                audioBuffer = [];
            } catch (e) {
                console.error("Audio processing error:", e);
            }
        }
        
        // Draw audio waveform on canvas
        function drawWaveform() {
            // Clear canvas
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            
            if (dataPoints.length === 0) return;
            
            // Find min and max values for dynamic scaling
            const min = Math.min(...dataPoints);
            const max = Math.max(...dataPoints);
            const range = Math.max(Math.abs(min), Math.abs(max), 1000);
            
            // Draw center line
            ctx.strokeStyle = '#ddd';
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(0, canvas.height / 2);
            ctx.lineTo(canvas.width, canvas.height / 2);
            ctx.stroke();
            
            // Draw waveform
            ctx.strokeStyle = isPlaying ? '#2ecc71' : '#3498db';
            ctx.lineWidth = 2;
            ctx.beginPath();
            
            const stepSize = canvas.width / (maxDataPoints - 1);
            for (let i = 0; i < dataPoints.length; i++) {
                const x = i * stepSize;
                const y = (canvas.height / 2) * (1 - dataPoints[i] / range);
                
                if (i === 0) {
                    ctx.moveTo(x, y);
                } else {
                    ctx.lineTo(x, y);
                }
            }
            
            ctx.stroke();
        }
        
        // Start audio playback
        function startAudio() {
            if (!audioContext && !initAudio()) {
                return;
            }
            
            // Resume the audio context if it was suspended
            if (audioContext.state === 'suspended') {
                audioContext.resume();
            }
            
            // Send play command to ESP32
            if (websocket && websocket.readyState === WebSocket.OPEN) {
                websocket.send('play');
            }
            
            isPlaying = true;
            playButton.disabled = true;
            stopButton.disabled = false;
            audioBuffer = [];
            audioIndicator.classList.add('audio-active');
        }
        
        // Stop audio playback
        function stopAudio() {
            // Send stop command to ESP32
            if (websocket && websocket.readyState === WebSocket.OPEN) {
                websocket.send('stop');
            }
            
            isPlaying = false;
            playButton.disabled = false;
            stopButton.disabled = true;
            audioBuffer = [];
            audioIndicator.classList.remove('audio-active');
        }
        
        // Initialize the application
        function init() {
            // Set up event listeners
            window.addEventListener('resize', resizeCanvas);
            playButton.addEventListener('click', startAudio);
            stopButton.addEventListener('click', stopAudio);
            
            // Initial canvas setup
            resizeCanvas();
            
            // Connect to WebSocket
            connectWebSocket();
        }
        
        // Run initialization when page loads
        window.onload = init;
    </script>
</body>
</html>
)rawliteral";
