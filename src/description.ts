/** An explicit empty string is intentional; null/undefined use package metadata. */
export const resolveDescription = (description: unknown, fallback: unknown): string => {
    const value = description ?? fallback ?? "";
    if (typeof value !== "string") {
        throw new Error("App description must be a string.");
    }
    return value;
};
