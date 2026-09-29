# Security

## Reporting a vulnerability

Please do not open a public issue for a security problem. Report it privately through GitHub's **Report a vulnerability** form on this repository's Security tab. You will get an acknowledgement within a few days and a fix or a timeline once the report is confirmed.

## Supported versions

Only the latest release receives fixes.

## What the app does with your data

- The device token is stored in Windows Credential Manager for this machine only and is sent to your Briareus server over HTTPS and nowhere else. HTTP, redirects and TLS below 1.2 are refused.
- Saved responses live under `%LOCALAPPDATA%\Okanet\Briareus\Responses`, encrypted with EFS where the volume allows it, and are erased when the connection is forgotten, revoked or replaced.
- Voice notes are sent to your server for transcription and to no other service.
- The app has no analytics, telemetry or automatic updates.
