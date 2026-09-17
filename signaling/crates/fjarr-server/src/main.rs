//! fjarr-server — the AGPL signaling sidecar. Deliberately thin: it mounts
//! the `fjarr-signaling` crate exactly the way a customer's Rust backend
//! would. spec: docs/09-interfaces.md#2-backend-tier--the-integration-contract-adr-0015

use std::net::SocketAddr;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // `fjarr-server --health`: container HEALTHCHECK probe with no extra
    // tooling in the runtime image (docker/signaling/Dockerfile).
    if std::env::args().any(|a| a == "--health") {
        return health_probe();
    }

    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "info".into()),
        )
        .init();

    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()?
        .block_on(serve())
}

async fn serve() -> Result<(), Box<dyn std::error::Error>> {
    let bind: SocketAddr = std::env::var("FJARR_BIND")
        .unwrap_or_else(|_| "0.0.0.0:8080".into())
        .parse()?;

    // Hooks from the environment (FJARR_GRANT_HS256_SECRET, FJARR_DEV_DEVICE_TOKEN,
    // FJARR_TURN_*, webhook settings): the sidecar's whole configuration
    // surface (docs/09 contract). Config::default() would reject everyone.
    let app =
        axum::Router::new().merge(fjarr_signaling::router(fjarr_signaling::Config::from_env()));

    tracing::info!(%bind, "fjarr-server listening");
    let listener = tokio::net::TcpListener::bind(bind).await?;
    axum::serve(listener, app)
        .with_graceful_shutdown(async {
            let _ = tokio::signal::ctrl_c().await;
            tracing::info!("shutting down");
        })
        .await?;
    Ok(())
}

/// Plain-std HTTP GET /healthz against ourselves; exit code is the probe.
fn health_probe() -> Result<(), Box<dyn std::error::Error>> {
    use std::io::{Read as _, Write as _};
    let port = std::env::var("FJARR_BIND")
        .ok()
        .and_then(|b| b.rsplit(':').next().map(str::to_owned))
        .unwrap_or_else(|| "8080".into());
    let mut stream = std::net::TcpStream::connect(("127.0.0.1", port.parse()?))?;
    stream.write_all(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")?;
    let mut response = String::new();
    stream.read_to_string(&mut response)?;
    if response.starts_with("HTTP/1.1 200") {
        Ok(())
    } else {
        Err(format!("unhealthy: {}", response.lines().next().unwrap_or("")).into())
    }
}
