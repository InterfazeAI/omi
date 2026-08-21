/**
 * Neutral dark palette. Purple is off-brand across Omi surfaces and must not
 * appear in icons, accents, glows, or gradients.
 */
export const colors = {
  background: '#000000',
  card: '#111113',
  cardBorder: '#26262a',
  text: '#ffffff',
  textMuted: '#9a9aa2',
  textFaint: '#5f5f68',
  accent: '#ffffff',
  accentText: '#000000',
  available: '#32d74b',
  idle: '#5f5f68',
  warning: '#ff9f0a',
  danger: '#ff453a',
  track: '#2a2a2f',
} as const;

export const spacing = {
  xs: 4,
  sm: 8,
  md: 12,
  lg: 16,
  xl: 24,
} as const;

export const radius = {
  sm: 8,
  md: 12,
  lg: 16,
} as const;
