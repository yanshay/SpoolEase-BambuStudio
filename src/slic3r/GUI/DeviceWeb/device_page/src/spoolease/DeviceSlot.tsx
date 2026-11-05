import './DeviceSlot.css';

export interface SpoolEaseSlotInfo {
  spool_id: string;
  short_id: string;
  weight: string;
}

export function normalizeSpoolEaseSlot(value: unknown): SpoolEaseSlotInfo | undefined {
  if (!value || typeof value !== 'object') return undefined;
  const info = value as Partial<SpoolEaseSlotInfo>;
  if (typeof info.spool_id !== 'string' || !info.spool_id ||
      typeof info.short_id !== 'string' || typeof info.weight !== 'string') return undefined;
  return { spool_id: info.spool_id, short_id: info.short_id, weight: info.weight };
}

export function spoolEaseSlotTooltip(base: string | undefined, info: SpoolEaseSlotInfo | undefined): string | undefined {
  if (!info) return base;
  return [base, '--------------------', '     SpoolEase', '--------------------',
    `Spool ID: ${info.spool_id}`, `Net weight: ${info.weight}`].filter(Boolean).join('\n');
}

export function SpoolEaseDeviceSlot({ info, inset }: {
  info: SpoolEaseSlotInfo | undefined;
  inset?: { left: number; right: number; top: number; bottom: number };
}) {
  if (!info) return null;
  return (
    <div className="spoolease-device-slot" aria-hidden="true" style={inset}>
      <span className="spoolease-device-slot-id">{info.short_id}</span>
      <span className="spoolease-device-slot-weight">{info.weight}</span>
    </div>
  );
}
