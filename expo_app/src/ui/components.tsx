import type { ReactNode } from 'react';
import {
  ActivityIndicator,
  Pressable,
  StyleSheet,
  Text,
  View,
  type ViewStyle,
} from 'react-native';

import { colors, radius, spacing } from './theme';

export function Card({ title, action, children }: { title: string; action?: ReactNode; children: ReactNode }) {
  return (
    <View style={styles.card}>
      <View style={styles.cardHeader}>
        <Text style={styles.cardTitle}>{title}</Text>
        {action}
      </View>
      {children}
    </View>
  );
}

export function Row({ label, value, tone }: { label: string; value: string; tone?: 'muted' | 'warning' }) {
  return (
    <View style={styles.row}>
      <Text style={styles.rowLabel}>{label}</Text>
      <Text
        style={[
          styles.rowValue,
          tone === 'muted' && { color: colors.textMuted },
          tone === 'warning' && { color: colors.warning },
        ]}
      >
        {value}
      </Text>
    </View>
  );
}

export function StatusDot({ active }: { active: boolean }) {
  return <View style={[styles.dot, { backgroundColor: active ? colors.available : colors.idle }]} />;
}

export function Button({
  label,
  onPress,
  disabled,
  busy,
  variant = 'primary',
  style,
}: {
  label: string;
  onPress: () => void;
  disabled?: boolean;
  busy?: boolean;
  variant?: 'primary' | 'secondary' | 'danger';
  style?: ViewStyle;
}) {
  const isDisabled = disabled || busy;
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityState={{ disabled: !!isDisabled, busy: !!busy }}
      onPress={onPress}
      disabled={isDisabled}
      style={({ pressed }) => [
        styles.button,
        variant === 'primary' && styles.buttonPrimary,
        variant === 'secondary' && styles.buttonSecondary,
        variant === 'danger' && styles.buttonDanger,
        isDisabled && styles.buttonDisabled,
        pressed && !isDisabled && styles.buttonPressed,
        style,
      ]}
    >
      {busy ? (
        <ActivityIndicator color={variant === 'primary' ? colors.accentText : colors.text} size="small" />
      ) : (
        <Text
          style={[
            styles.buttonLabel,
            variant === 'primary' && { color: colors.accentText },
            variant === 'danger' && { color: colors.danger },
          ]}
        >
          {label}
        </Text>
      )}
    </Pressable>
  );
}

export function ProgressBar({ value }: { value: number }) {
  const clamped = Math.max(0, Math.min(1, Number.isFinite(value) ? value : 0));
  return (
    <View style={styles.track}>
      <View style={[styles.fill, { width: `${clamped * 100}%` }]} />
    </View>
  );
}

export function Hint({ children, tone }: { children: ReactNode; tone?: 'warning' | 'danger' }) {
  return (
    <Text
      style={[
        styles.hint,
        tone === 'warning' && { color: colors.warning },
        tone === 'danger' && { color: colors.danger },
      ]}
    >
      {children}
    </Text>
  );
}

const styles = StyleSheet.create({
  card: {
    backgroundColor: colors.card,
    borderColor: colors.cardBorder,
    borderWidth: StyleSheet.hairlineWidth,
    borderRadius: radius.lg,
    padding: spacing.lg,
    marginBottom: spacing.lg,
  },
  cardHeader: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    marginBottom: spacing.md,
  },
  cardTitle: {
    color: colors.textMuted,
    fontSize: 12,
    fontWeight: '600',
    letterSpacing: 1.1,
    textTransform: 'uppercase',
  },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingVertical: spacing.xs + 1,
  },
  rowLabel: {
    color: colors.textMuted,
    fontSize: 14,
  },
  rowValue: {
    color: colors.text,
    fontSize: 14,
    fontVariant: ['tabular-nums'],
  },
  dot: {
    width: 8,
    height: 8,
    borderRadius: 4,
  },
  button: {
    height: 46,
    borderRadius: radius.md,
    alignItems: 'center',
    justifyContent: 'center',
    paddingHorizontal: spacing.lg,
  },
  buttonPrimary: {
    backgroundColor: colors.accent,
  },
  buttonSecondary: {
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.cardBorder,
    backgroundColor: 'transparent',
  },
  buttonDanger: {
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.danger,
    backgroundColor: 'transparent',
  },
  buttonDisabled: {
    opacity: 0.4,
  },
  buttonPressed: {
    opacity: 0.75,
  },
  buttonLabel: {
    color: colors.text,
    fontSize: 15,
    fontWeight: '600',
  },
  track: {
    height: 4,
    borderRadius: 2,
    backgroundColor: colors.track,
    overflow: 'hidden',
  },
  fill: {
    height: '100%',
    backgroundColor: colors.accent,
  },
  hint: {
    color: colors.textFaint,
    fontSize: 12,
    lineHeight: 17,
    marginTop: spacing.sm,
  },
});
