# Editor

## Overview
This project is an open-source template created by Olivenda. It is designed to help developers streamline their workflows and create efficient solutions quickly.

## License

This project is licensed under the [MIT License](LICENSE), with the following conditions:

1. **Attribution**: If you use this template, you must credit **Olivenda** as the original creator of the template in your project’s documentation.
   
2. **Non-Commercial Use**: You are **not allowed** to sell this template or any project derived from it. It is intended for personal or collaborative use only. Commercial usage, distribution, or resale is strictly prohibited.

3. **Open-Source**: You may modify, distribute, and use this template as long as you adhere to the above conditions.

## Launching the Editor

1. Compile the editor using the provided `Makefile`:
   ```bash
   make
   ```
2. Run the editor with the file you want to edit:
   ```bash
   ./quill <filename>
   ```

## Key Bindings

The following shortcuts are available during editing:

- **Ctrl+X** – save the current file and exit.
- **Ctrl+S** – save without exiting.
- **Ctrl+Q** – quit the editor. If there are unsaved changes you will be prompted to press **Ctrl+Q** again.
- **Arrow keys** – navigate within the document.

The status bar lists additional keys (**Ctrl+F** for search and **Ctrl+G** for jumping to a line), though these features are currently unimplemented.

## Update Script Security

The `update.sh` script downloads code from the internet using `wget` and installs it on your system. Review the script before running it to ensure you are comfortable with the operations it performs.


   
For any questions or inquiries, feel free to reach out to Olivenda via [Priviliq5@gmail.com].
© 2025 Olivenda. All Rights Reserved.

## Build
Run `make` to compile the editor. The source code is located in the `src/` directory.
