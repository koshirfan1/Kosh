# Kosh Portfolio Project

This project is a small personal portfolio website built with the Kosh language and the `webi` web framework.

## Features

- Modern single-page portfolio layout
- Hero section, about section, skills, and contact area
- Responsive design for desktop and mobile devices
- Runs as a local web app using Kosh + webi
- Accessible from other devices on the same network using the host machine IP address

## Run it

From this folder:

```powershell
kosh server.kh
```

Then open:

```text
http://localhost:8099/
```

To access it from another system on the same network, use the machine's local IP address instead of `localhost`:

```text
http://192.168.1.10:8099/
```

Replace `192.168.1.10` with the actual IP of the machine running the app.

## Files

- `server.kh` — app entry point and route setup
- `templates/portfolio.html` — portfolio page template

## Notes

The server binds to `0.0.0.0`, so it is reachable across the LAN as well as locally.
