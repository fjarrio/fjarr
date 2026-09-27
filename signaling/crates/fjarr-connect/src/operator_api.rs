//! The customer's operator API, as the CLI sees it (docs/09#operator-api): the robots this human
//! may reach, a grant for one of them, and the login-code trio for a terminal with no browser.
//!
//! Everything here is authenticated with the credential `login` obtained and nothing else. The
//! CLI never holds the tenant's control-plane token and never calls fjarr-server's REST API: that
//! token is a fleet-wide administrative credential and belongs on a server, not on a laptop that
//! travels (docs/27#discovery).
use anyhow::{anyhow, Context, Result};
use serde::Deserialize;

#[derive(Debug, Clone, Deserialize)]
pub struct Robot {
    pub robot_id: String,
    #[serde(default)]
    pub label: String,
    #[serde(default = "unknown")]
    pub status: String,
    /// Milliseconds since the epoch, when the backend knows.
    #[serde(default)]
    pub last_seen: Option<u64>,
}

fn unknown() -> String {
    "unknown".to_string()
}

impl Robot {
    /// The spec's filter: the id exactly, or a case-insensitive substring of the id or the label,
    /// so `fjarr-connect packer3` works and nobody memorises identifiers (docs/27#what-it-feels-like).
    pub fn matches(&self, needle: &str) -> bool {
        if self.robot_id == needle {
            return true;
        }
        let n = needle.to_lowercase();
        self.robot_id.to_lowercase().contains(&n) || self.label.to_lowercase().contains(&n)
    }
}

#[derive(Debug, Deserialize)]
pub struct CliCode {
    pub code: String,
    pub poll_token: String,
    #[serde(default)]
    pub expires_in: u64,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "status", rename_all = "lowercase")]
pub enum Poll {
    Pending,
    Approved { credential: String },
    Expired,
}

pub struct Client {
    http: reqwest::Client,
    base: String,
    credential: Option<String>,
}

impl Client {
    /// `base` is where the operator API is mounted (`[backend] url`), without a trailing slash.
    pub fn new(base: &str, credential: Option<String>) -> Result<Self> {
        let http = reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(15))
            .build()
            .context("building the HTTP client")?;
        Ok(Self {
            http,
            base: base.trim_end_matches('/').to_string(),
            credential,
        })
    }

    fn authed(&self, req: reqwest::RequestBuilder) -> Result<reqwest::RequestBuilder> {
        let cred = self.credential.as_deref().ok_or_else(|| {
            anyhow!("not logged in — run `fjarr-connect login <dashboard url>` first")
        })?;
        Ok(req.bearer_auth(cred))
    }

    /// A refusal, in the backend's words: 401 is "log in again", anything else is what they said.
    async fn read<T: serde::de::DeserializeOwned>(
        &self,
        what: &str,
        res: reqwest::Response,
    ) -> Result<T> {
        let status = res.status();
        let text = res.text().await.unwrap_or_default();
        if status == reqwest::StatusCode::UNAUTHORIZED {
            anyhow::bail!("{what}: the backend no longer accepts this credential — run `fjarr-connect login` again");
        }
        if !status.is_success() {
            let message = serde_json::from_str::<serde_json::Value>(&text)
                .ok()
                .and_then(|v| v.get("error").and_then(|e| e.as_str()).map(String::from))
                .unwrap_or_else(|| text.chars().take(200).collect());
            anyhow::bail!("{what}: {status} {message}");
        }
        serde_json::from_str(&text).with_context(|| {
            format!(
                "{what}: unexpected reply {}",
                text.chars().take(200).collect::<String>()
            )
        })
    }

    pub async fn robots(&self) -> Result<Vec<Robot>> {
        let req = self.authed(self.http.get(format!("{}/fjarr/robots", self.base)))?;
        let res = req
            .send()
            .await
            .with_context(|| format!("reaching {}", self.base))?;
        self.read("listing robots", res).await
    }

    pub async fn grant(&self, robot_id: &str) -> Result<String> {
        #[derive(Deserialize)]
        struct Reply {
            grant: String,
        }
        let req = self.authed(
            self.http
                .post(format!("{}/fjarr/grants", self.base))
                .json(&serde_json::json!({ "robot_id": robot_id })),
        )?;
        let res = req
            .send()
            .await
            .with_context(|| format!("reaching {}", self.base))?;
        Ok(self
            .read::<Reply>(&format!("a grant for {robot_id}"), res)
            .await?
            .grant)
    }

    pub async fn create_code(&self) -> Result<CliCode> {
        let res = self
            .http
            .post(format!("{}/fjarr/cli-codes", self.base))
            .send()
            .await
            .with_context(|| format!("reaching {}", self.base))?;
        self.read("asking for a login code", res).await
    }

    pub async fn poll_code(&self, poll_token: &str) -> Result<Poll> {
        let res = self
            .http
            .get(format!("{}/fjarr/cli-codes/{poll_token}", self.base))
            .send()
            .await
            .with_context(|| format!("reaching {}", self.base))?;
        self.read("polling the login code", res).await
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn robot(id: &str, label: &str) -> Robot {
        Robot {
            robot_id: id.into(),
            label: label.into(),
            status: "online".into(),
            last_seen: None,
        }
    }

    #[test]
    fn the_filter_matches_id_or_label_case_insensitively() {
        let r = robot("robot-024", "Packer 3 · Malmo");
        assert!(r.matches("robot-024"));
        assert!(r.matches("packer 3"));
        assert!(r.matches("MALMO"));
        assert!(r.matches("024"));
        assert!(!r.matches("robot-031"));
        assert!(!r.matches("Lund"));
    }

    #[test]
    fn a_poll_reply_is_one_of_three_things() {
        assert!(matches!(
            serde_json::from_str::<Poll>(r#"{"status":"pending"}"#).unwrap(),
            Poll::Pending
        ));
        assert!(matches!(
            serde_json::from_str::<Poll>(r#"{"status":"expired"}"#).unwrap(),
            Poll::Expired
        ));
        match serde_json::from_str::<Poll>(r#"{"status":"approved","credential":"c"}"#).unwrap() {
            Poll::Approved { credential } => assert_eq!(credential, "c"),
            other => panic!("{other:?}"),
        }
    }
}
