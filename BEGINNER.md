# Beginner's Guide to Cypher Jammer

Welcome to the **Cypher Jammer** project. This repository contains open-source hardware designs and firmware that generate wireless noise in the 2.4 GHz band for penetration-testing and research purposes. The core of the project is an ESP32 development board paired with one or two NRF24L01+PA+LNA transceivers.

Cypher Jammer is powerful and must be used responsibly. This guide gives newcomers a quick overview of the project, points to useful resources, and highlights important safety considerations.

## Repository Overview
- **`cypher-jammer/cypher-jammer.ino`** – main firmware that hops across channels and emits noise.
- **`smoochie_version/`** – alternative sketches for single, dual, or user-controlled channel modes.
- **`web/`** – precompiled binaries and manifests for the browser-based flasher.
- **`hardware/`** – schematic, PCB artwork, and Gerber files.

The root `README.md` contains detailed assembly photos, wiring diagrams, and additional notes. Read it carefully before attempting to build or flash anything.

## Legal & Ethical Warnings
- **Wireless jamming is illegal in many jurisdictions.** Only transmit when you have explicit authorization from the spectrum owner (e.g., in a shielded lab or with your own equipment).
- Do **not** disrupt emergency services, public infrastructure, or other critical communications.
- Respect local radio regulations, licensing requirements, and airspace restrictions.
- Use the device strictly for education, research, or defensive security testing.
- You are solely responsible for any damage, fines, or legal consequences arising from misuse.

## Hardware Safety
- Provide stable power to the ESP32 and NRF24 modules; unstable voltage can damage components.
- Add a decoupling capacitor (10µF–100µF) close to each NRF24 module as recommended in the README.
- Avoid operating high-power amplifiers near people or sensitive electronics.
- Keep antennas and RF amplifiers away from your body to minimize exposure.

## Getting Started
1. Acquire the required hardware listed in `README.md` (ESP32, NRF24 modules, capacitor, optional switch).
2. Review the wiring diagrams in the `hardware/` folder and the README.
3. For a quick start, use the web flasher (`flash1.html`) and select the binary that matches your hardware setup. Ensure you use a Chromium-based browser.
4. If compiling yourself, install the Arduino IDE or `arduino-cli`, add the ESP32 board package, and install the `RF24` and `ezButton` libraries before building any `.ino` file.
5. Test your setup on a private, controlled network before experimenting elsewhere.

## Tips for Newcomers
- Start with a single NRF24 module to simplify debugging, then expand to dual modules if needed.
- Experiment with low transmit power and short range to understand behavior.
- Keep logs of your experiments and network conditions.
- Learn basic RF concepts (channels, power levels, interference) to interpret results correctly.
- Contribute improvements or documentation via pull requests, giving credit to original authors.

## Further Resources
- [RF24 Library](https://github.com/nRF24/RF24)
- [ezButton Library](https://arduinogetstarted.com/tutorials/arduino-button-library)
- [Noisy Boy Project](https://github.com/smoochiee/Noisy-boy-esp32-Bluetooth-jammer)

Stay safe, experiment responsibly, and enjoy learning about wireless security!
