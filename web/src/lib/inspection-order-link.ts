/** Preserve exchange IDs exactly, including values above JavaScript's integer precision. */
export function initialOrderId(order: string | null): string | null {
  return order !== null && /^(0|[1-9][0-9]*)$/.test(order) ? order : null;
}
