/**
 * Signaling socket seam. The default wraps a browser WebSocket; tests inject
 * a scripted fake (docs/15 fault injection).
 * spec: docs/08-protocol.md#signaling · ADR-0013
 */

export interface SignalingSocket {
  send(text: string): void;
  close(): void;
  onopen: (() => void) | null;
  onmessage: ((text: string) => void) | null;
  /** Called exactly once when the socket is gone, however it went. */
  onclose: ((reason: string) => void) | null;
}

export type SocketFactory = (url: string) => SignalingSocket;

export const webSocketFactory: SocketFactory = (url) => {
  const ws = new WebSocket(url);
  let closed = false;
  const socket: SignalingSocket = {
    send: (text) => {
      if (ws.readyState === WebSocket.OPEN) ws.send(text);
    },
    close: () => {
      try {
        ws.close();
      } catch {
        /* already closed */
      }
    },
    onopen: null,
    onmessage: null,
    onclose: null,
  };
  const finish = (reason: string) => {
    if (closed) return;
    closed = true;
    socket.onclose?.(reason);
  };
  ws.onopen = () => socket.onopen?.();
  ws.onmessage = (ev) => {
    if (typeof ev.data === "string") socket.onmessage?.(ev.data);
  };
  // Browsers fire `close` (with the code) right after `error`; let it carry
  // the reason, and fall back to a generic one only if it never arrives.
  ws.onerror = () => {
    setTimeout(() => finish("socket-error"), 250);
  };
  ws.onclose = (ev) => finish(ev.reason || `socket-closed:${ev.code}`);
  return socket;
};
