// Node handles TTY detection, NO_COLOR and FORCE_COLOR without a logging dependency.
import { styleText } from "node:util";

export default {
    "red": (value: unknown): string => styleText("red", String(value)),
    "green": (value: unknown): string => styleText("green", String(value)),
    "yellow": (value: unknown): string => styleText("yellow", String(value))
};
