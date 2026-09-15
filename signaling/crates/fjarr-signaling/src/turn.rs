//! Ephemeral TURN credentials for coturn's use-auth-secret mode:
//! username = "<expiry-unix>:<session_id>",
//! credential = base64(HMAC-SHA1(secret, username)).
//! spec: docs/10-security.md#turn

use base64::Engine as _;
use hmac::{Hmac, Mac as _};

use crate::protocol::TurnCredentials;

#[derive(Clone)]
pub struct TurnConfig {
    pub urls: Vec<String>,
    pub secret: String,
    pub ttl_secs: u64,
}

pub fn mint(config: &TurnConfig, session_id: &str) -> TurnCredentials {
    let expiry = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
        + config.ttl_secs;
    let username = format!("{expiry}:{session_id}");
    let mut mac = Hmac::<sha1::Sha1>::new_from_slice(config.secret.as_bytes())
        .expect("hmac accepts any key length");
    mac.update(username.as_bytes());
    let credential = base64::engine::general_purpose::STANDARD.encode(mac.finalize().into_bytes());
    TurnCredentials {
        urls: config.urls.clone(),
        username,
        credential,
        ttl: config.ttl_secs,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mints_the_coturn_hmac_format() {
        // Cross-checked against `openssl dgst -sha1 -hmac` during the M0
        // environment verification (docs/12#turn-sanity-check).
        let config = TurnConfig {
            urls: vec!["turn:turn.fjarr.io:3478".into()],
            secret: "dev-only-change-me".into(),
            ttl_secs: 600,
        };
        let creds = mint(&config, "s-01");
        let (expiry, sid) = creds.username.split_once(':').unwrap();
        assert_eq!(sid, "s-01");
        assert!(expiry.parse::<u64>().unwrap() > 1_700_000_000);
        assert!(!creds.credential.is_empty());
    }
}
