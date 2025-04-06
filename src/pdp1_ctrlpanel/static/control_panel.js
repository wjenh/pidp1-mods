// Reader functions
async function mountTape() {
    const fileInput = document.getElementById('tapeFile');
    const status = document.getElementById('readerStatus');

    if (!fileInput.files[0]) {
        status.textContent = 'Please select a file first';
        return;
    }

    const formData = new FormData();
    formData.append('tape', fileInput.files[0]);

    try {
        const response = await fetch('/api/reader/mount', {
            method: 'POST',
            body: formData
        });
        const text = await response.text();
        status.textContent = text || 'Mounted successfully';
    } catch (error) {
        status.textContent = 'Error: ' + error.message;
    }
}

async function unmountTape() {
    const status = document.getElementById('readerStatus');
    try {
        const response = await fetch('/api/reader/unmount', {
            method: 'POST'
        });
        const text = await response.text();
        status.textContent = text || 'Unmounted successfully';
    } catch (error) {
        status.textContent = 'Error: ' + error.message;
    }
}

// Punch functions
async function initPunch() {
    const status = document.getElementById('punchStatus');
    try {
        const response = await fetch('/api/punch/init', {
            method: 'POST'
        });
        const text = await response.text();
        status.textContent = text || 'Punch initialized';
    } catch (error) {
        status.textContent = 'Error: ' + error.message;
    }
}

async function getPunchedTape() {
    const status = document.getElementById('punchStatus');
    try {
        window.location.href = '/api/punch/get';
        status.textContent = 'Download started';
    } catch (error) {
        status.textContent = 'Error: ' + error.message;
    }
}

// Hardware option functions
async function queryMulDivState() {
    try {
        const response = await fetch('/api/muldiv/query');
        const text = await response.text();
        const isOn = text.includes('on');
        document.getElementById('muldivToggle').checked = isOn;
        document.getElementById('muldivStatus').textContent = isOn ? 'ON' : 'OFF';
    } catch (error) {
        console.error('Error querying mul-div state:', error);
    }
}

async function toggleMulDiv() {
    const toggle = document.getElementById('muldivToggle');
    const status = document.getElementById('muldivStatus');
    const newState = toggle.checked ? 'on' : 'off';

    try {
        const formData = new FormData();
        formData.append('state', newState);

        const response = await fetch('/api/muldiv/set', {
            method: 'POST',
            body: formData
        });

        const text = await response.text();
        const isOn = text.includes('on');
        toggle.checked = isOn;
        status.textContent = isOn ? 'ON' : 'OFF';
    } catch (error) {
        console.error('Error setting mul-div state:', error);
        status.textContent = 'ERROR';
    }
}

// Assembler functions
let currentFileName = '';

async function assembleFile() {
    const fileInput = document.getElementById('sourceFile');
    const status = document.getElementById('assembleStatus');
    const output = document.getElementById('assembleOutput');
    const results = document.getElementById('assembleResults');
    
    if (!fileInput.files[0]) {
        status.textContent = 'Please select a file first';
        return;
    }

    status.textContent = 'Assembling...';
    output.textContent = '';
    results.style.display = 'none';

    const originalFile = fileInput.files[0];
    const fileContent = await originalFile.text();
    const newFile = new File(
        [fileContent],
        originalFile.name,
        {
            type: originalFile.type,
            lastModified: Date.now()
        }
    );

    const formData = new FormData();
    formData.append('source', newFile);

    try {
        const response = await fetch('/api/assemble', {
            method: 'POST',
            body: formData
        });
        
        let result;
        const text = await response.text();
        try {
            result = JSON.parse(text);
        } catch (e) {
            console.log('Raw response:', text);
            throw new Error('Invalid response from server: ' + text);
        }
        
        if (result.error) {
            status.textContent = 'Assembly failed';
            output.textContent = result.error;
        } else {
            status.textContent = 'Assembly completed';
            currentFileName = originalFile.name.replace('.mac', '');
            results.style.display = 'block';
            
            document.getElementById('lstLink').href = `/api/lst?file=${encodeURIComponent(currentFileName)}`;
            document.getElementById('rimLink').href = `/api/rim?file=${encodeURIComponent(currentFileName)}`;
        }
    } catch (error) {
        console.error('Assembly error:', error);
        status.textContent = 'Error: ' + error.message;
        output.textContent = 'Failed to process assembly request';
    }
}

async function mountRim() {
    if (!currentFileName) return;
    const status = document.getElementById('readerStatus');
    
    try {
        const response = await fetch(`/api/rim?file=${encodeURIComponent(currentFileName)}`);
        const blob = await response.blob();
        
        const formData = new FormData();
        formData.append('tape', new File([blob], currentFileName + '.rim'));
        
        const mountResponse = await fetch('/api/reader/mount', {
            method: 'POST',
            body: formData
        });
        
        const text = await mountResponse.text();
        status.textContent = text || 'RIM file mounted successfully';
    } catch (error) {
        status.textContent = 'Error mounting RIM file: ' + error.message;
    }
}

// Initialize hardware state on load
document.addEventListener('DOMContentLoaded', queryMulDivState);
