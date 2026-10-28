/* The native build embeds ui/index.html into the binary (tools/embed,
 * build/ui/ui_page.c) for --ui to serve.  Neither WASM build serves a page:
 * the site ships it, and the Node build runs headless.  An empty one keeps
 * websocket_ui_service.c the same code as native. */
const unsigned char csim_ui_page[] = "";
const unsigned int csim_ui_page_len = 0;
