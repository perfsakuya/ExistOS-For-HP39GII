// Source-level input contract for the Pocket Vapor compiler. The native
// adapter dispatches button edges; this stub is not a browser simulator.
export const Button = { A: 0, B: 1, Select: 2, Start: 3, Right: 4, Left: 5, Up: 6, Down: 7, R: 8, L: 9 } as const;
export function onButton(_handler: (button: number) => void): void {}
