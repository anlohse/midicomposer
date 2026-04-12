import '../components/app-root';
import { CoreBridge } from '../bridge/coreBridge';

// Redirect console logs to Core
const originalLog = console.log;
const originalWarn = console.warn;
const originalError = console.error;

let isLogging = false;

console.log = (...args) => {
    originalLog(...args);
    if (isLogging) return;
    isLogging = true;
    CoreBridge.sendCommand('ui_log', { level: 'info', message: args.map(a => String(a)).join(' ') })
        .finally(() => isLogging = false);
};
console.warn = (...args) => {
    originalWarn(...args);
    if (isLogging) return;
    isLogging = true;
    CoreBridge.sendCommand('ui_log', { level: 'warn', message: args.map(a => String(a)).join(' ') })
        .finally(() => isLogging = false);
};
console.error = (...args) => {
    originalError(...args);
    if (isLogging) return;
    isLogging = true;
    CoreBridge.sendCommand('ui_log', { level: 'error', message: args.map(a => String(a)).join(' ') })
        .finally(() => isLogging = false);
};

console.log('MIDI Composer UI Initialized');
