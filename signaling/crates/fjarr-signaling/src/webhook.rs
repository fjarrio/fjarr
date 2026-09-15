//! HMAC-signed webhook delivery to the customer's backend.
//! spec: docs/09-interfaces.md#b-webhooks-fjarr-server--customer-backend
//! At-least-once with bounded retries; idempotency via event_id.

use hmac::{Hmac, Mac as _};
use serde_json::{json, Value};

use crate::hooks::{Event, EventSink};
use crate::protocol::now_ms;

pub struct WebhookSink {
    url: String,
    secret: String,
    client: reqwest::Client,
}

impl WebhookSink {
    pub fn new(url: String, secret: String) -> Self {
        Self {
            url,
            secret,
            client: reqwest::Client::new(),
        }
    }
}

impl EventSink for WebhookSink {
    fn emit(&self, event: Event, data: Value) {
        let body = json!({
            "event": event.name(),
            "event_id": uuid::Uuid::now_v7().to_string(),
            "ts": now_ms(),
            "data": data,
        })
        .to_string();
        let mut mac = Hmac::<sha2::Sha256>::new_from_slice(self.secret.as_bytes())
            .expect("hmac accepts any key length");
        mac.update(body.as_bytes());
        let signature = format!(
            "sha256={}",
            mac.finalize()
                .into_bytes()
                .iter()
                .map(|b| format!("{b:02x}"))
                .collect::<String>()
        );

        let url = self.url.clone();
        let client = self.client.clone();
        tokio::spawn(async move {
            for (attempt, delay_secs) in [(1u8, 1u64), (2, 5), (3, 0)] {
                let sent = client
                    .post(&url)
                    .header("content-type", "application/json")
                    .header("x-fjarr-signature", &signature)
                    .body(body.clone())
                    .send()
                    .await;
                match sent {
                    Ok(response) if response.status().is_success() => return,
                    Ok(response) => tracing::warn!(
                        %url, attempt, status = %response.status(), "webhook non-2xx"
                    ),
                    Err(error) => tracing::warn!(%url, attempt, %error, "webhook send failed"),
                }
                if delay_secs > 0 {
                    tokio::time::sleep(std::time::Duration::from_secs(delay_secs)).await;
                }
            }
            tracing::error!(%url, "webhook dropped after 3 attempts");
        });
    }
}
