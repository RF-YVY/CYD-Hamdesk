const status = document.querySelector("#build-status");
try {
  const response = await fetch("build.json", { cache: "no-store" });
  if (!response.ok) throw new Error("Firmware metadata unavailable");
  const build = await response.json();
  // Refuse to offer a button until both manifests and their images are present.
  for (const installer of document.querySelectorAll("[data-installer]")) {
    const manifestUrl = new URL(installer.getAttribute("manifest"), location.href);
    const result = await fetch(manifestUrl, { cache: "no-store" });
    if (!result.ok) throw new Error("Firmware manifest unavailable");
    const manifest = await result.json();
    for (const part of manifest.builds[0].parts) {
      const image = await fetch(new URL(part.path, manifestUrl), { method: "HEAD" });
      if (!image.ok) throw new Error("Firmware download unavailable");
    }
  }
  await customElements.whenDefined("esp-web-install-button");
  document.querySelectorAll("button[slot='activate']").forEach(button => button.disabled = false);
  status.textContent = `Firmware ${build.version} · ${build.board} · SHA-256 checksums included with downloads`;
} catch (error) {
  status.textContent = "Firmware is unavailable. Please reload after the GitHub build finishes, or use the setup guide to build locally.";
  console.error(error);
}
