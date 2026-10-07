# Browser Automation & Web Retrieval Guidelines

1. **Tool Hierarchy for Web Information Retrieval**:
   - **Static Content & Documentation**:
     - Use `read_url_content` for documentation, markdown files, GitHub issues, and static pages. It is fast, lightweight, and requires no browser process.
   - **Interactive & Dynamic / JavaScript-Heavy Sites**:
     - Use `agent-browser` as the primary CLI browser automation tool.
     - Usage pattern:
       ```bash
       agent-browser open "<URL>" && agent-browser snapshot -i && agent-browser close
       ```
     - Benefits: Produces a clean, semantic accessibility tree directly on `stdout`, supports interactive element refs (`@e1`, `@e2`), operates in headless mode, and does not pollute the git repository with tracking or cache files.
   - **Raw Headless HTML Dumps**:
     - Use `google-chrome --headless --disable-gpu --dump-dom "<URL>"` when the raw DOM HTML is needed.
   - **Playwright CLI (`playwright-cli`)**:
     - Note: `playwright-cli` writes `.playwright-cli/` directories and session YAML files to the current working directory.
     - When using `playwright-cli`, always ensure any generated artifacts in the workspace are immediately cleaned up, or set the working directory to the scratch folder (`<appDataDir>/brain/<conversation-id>/scratch/`).
   - **Full Interactive Browser Sessions**:
     - Use the Antigravity `browser_subagent` tool when visual interaction, screenshots, or multi-step form navigation are required.
