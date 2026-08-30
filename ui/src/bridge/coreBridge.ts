export interface BridgeResponse<T = any> {
    success: boolean;
    result?: T;
    error?: string;
}

export interface BridgeMessage {
    type: string;
    payload?: any;
}

export type BridgeEventHandler = (payload: any) => void;

export class CoreBridge {
    private static eventHandlers: Map<string, BridgeEventHandler[]> = new Map();

    /**
     * Finds the bridge function in various possible Saucer 2.x locations.
     */
    private static getBridgeFunc(): Function | null {
        const w = window as any;
        return w.saucer?.call || 
               w.saucer?.exposed?.send_to_core || 
               w.send_to_core || 
               null;
    }

    static isNative(): boolean {
        const w = window as any;
        return !!(w.is_native_host || this.getBridgeFunc());
    }

    /**
     * Waits for the bridge to be injected by the host.
     */
    static async waitForBridge(timeoutMs: number = 2000): Promise<boolean> {
        const start = Date.now();
        while (Date.now() - start < timeoutMs) {
            if (this.isNative()) {
                console.log('[CoreBridge] Bridge detected!');
                return true;
            }
            await new Promise(resolve => setTimeout(resolve, 50));
        }
        return false;
    }

    static async sendCommand<T = any>(type: string, payload: any = {}): Promise<T> {
        const bridgeFunc = this.getBridgeFunc();
        const saucer = (window as any).saucer;

        // Try saucer.call first if available (Saucer 2.x standard)
        if (saucer && typeof saucer.call === 'function') {
            const message: BridgeMessage = { type, payload };
            try {
                const responseJson = await saucer.call('send_to_core', [JSON.stringify(message)]);
                const response: BridgeResponse<T> = typeof responseJson === 'string' 
                    ? JSON.parse(responseJson) 
                    : responseJson;

                if (!response.success) {
                    throw new Error(response.error || 'Unknown core error');
                }
                return response.result as T;
            } catch (err) {
                console.error('Saucer call failed:', err);
                throw err;
            }
        }

        // Fallback to direct exposed function if present
        if (typeof bridgeFunc === 'function') {
            const message: BridgeMessage = { type, payload };
            const responseJson = await (bridgeFunc as any)(JSON.stringify(message));
            const response: BridgeResponse<T> = typeof responseJson === 'string' 
                ? JSON.parse(responseJson) 
                : responseJson;

            if (!response.success) {
                throw new Error(response.error || 'Unknown core error');
            }
            return response.result as T;
        }

        if (type !== 'ui_log') {
            console.warn('Bridge not available (Mock Mode). Command:', type);
        }
        return this.mockResponse(type);
    }

    static on(eventType: string, handler: BridgeEventHandler) {
        if (!this.eventHandlers.has(eventType)) {
            this.eventHandlers.set(eventType, []);
        }
        this.eventHandlers.get(eventType)!.push(handler);
    }

    static off(eventType: string, handler: BridgeEventHandler) {
        const handlers = this.eventHandlers.get(eventType);
        if (!handlers) return;
        const idx = handlers.indexOf(handler);
        if (idx >= 0) handlers.splice(idx, 1);
    }

    static dispatchNativeEvent(eventType: string, payload: any) {
        // The native host serializes the payload as a JSON string when it
        // interpolates evaluate() arguments — decode it back to an object.
        if (typeof payload === 'string') {
            try { payload = JSON.parse(payload); } catch { /* keep as-is */ }
        }
        const handlers = this.eventHandlers.get(eventType);
        if (handlers) {
            handlers.forEach(h => h(payload));
        }
    }

    private static mockResponse(type: string): any {
        // Deliberately 0.0.0: a mock that looks like a release number drifts
        // out of date exactly like a hardcoded one, with nothing to catch it.
        if (type === 'get_version') return '0.0.0-mock';
        if (type === 'ping') return 'pong-mock';
        if (type === 'get_open_documents') return [];
        return {};
    }
}

(window as any).dispatchNativeEvent = (type: string, payload: any) => CoreBridge.dispatchNativeEvent(type, payload);
