const urlParams = new URLSearchParams(window.location.search);
const next = urlParams.get("next");

if (next !== null) {
    await import("./next/main.ts");
} else {
    await import("./main.ts");
}
