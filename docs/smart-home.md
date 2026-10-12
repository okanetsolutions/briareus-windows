# Control Home Assistant

1. Configure `HOME_ASSISTANT_URL` and `HOME_ASSISTANT_TOKEN` on the Briareus server.
2. Connect the Windows client with an Admin token.
3. Open **Settings → Smart home**. This entry appears when the server advertises the Home Assistant API.
4. On **Entities**, select a domain filter or leave it empty to list every entity. Select an entity to read its attributes and timestamps. Entity states refresh every ten seconds while this screen is visible and the app is active.
5. On **Actions**, select a service. Review its description and fields, then choose **Edit action data** to enter a JSON object. Selecting an entity first prepopulates `entity_id`, for example `{"entity_id":"light.kitchen"}`.
6. For a service that requires response data, enable **Response data: requested**. The client enables this by default when the service definition marks its response as required. Leave it disabled for services that do not support responses.
7. Choose **Run action**, then inspect the receipt and current entity state. A successful receipt does not prove that the physical device completed the action.
8. To trigger an automation event, open **Events**, choose its type, edit its JSON data, and choose **Fire event**. Acceptance does not confirm that an automation finished.

If a write times out or returns an invalid receipt, the client does not retry it. Read the entity state before sending another action. The client refreshes states after each write and disables further writes after an uncertain result until it receives an entity list. If that read fails, use **Refresh** after resolving the connection problem.

The server's connection status distinguishes an unconfigured integration from a disconnected one. The client displays API errors, including missing routes, unavailable upstream services, and permission failures. Home Assistant credentials remain on the server.

The implementation follows the [core API contract](https://github.com/nadinyamaui/briareus/blob/main/docs/api-v1-reference.md#home-assistant).
