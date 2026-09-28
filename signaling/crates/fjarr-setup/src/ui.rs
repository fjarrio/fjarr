//! Every prompt has a flag (docs/26#the-setup-tool): a value given on the command line is shown as
//! a settled step, a missing one is asked for on a terminal, and without a terminal the error names
//! the flag a provisioning script has to pass.
use anyhow::{anyhow, Result};

pub fn is_terminal() -> bool {
    // SAFETY: isatty has no preconditions.
    unsafe { libc::isatty(libc::STDIN_FILENO) == 1 && libc::isatty(libc::STDERR_FILENO) == 1 }
}

fn no_terminal(flag: &str, question: &str) -> anyhow::Error {
    anyhow!("no terminal to ask \"{question}\": pass {flag}")
}

/// A yes/no with a flag: `given` when the flag was passed, `--yes` accepts the default, else a prompt.
pub fn confirm(
    question: &str,
    flag: &str,
    given: Option<bool>,
    yes: bool,
    default: bool,
) -> Result<bool> {
    if let Some(v) = given {
        cliclack::log::step(format!("{question}  {}", if v { "yes" } else { "no" }))?;
        return Ok(v);
    }
    if yes {
        cliclack::log::step(format!(
            "{question}  {} (--yes)",
            if default { "yes" } else { "no" }
        ))?;
        return Ok(default);
    }
    if !is_terminal() {
        return Err(no_terminal(flag, question));
    }
    Ok(cliclack::confirm(question)
        .initial_value(default)
        .interact()?)
}

/// One of several, each with a label and a hint.
pub fn select<T: Clone + Eq>(
    question: &str,
    flag: &str,
    given: Option<T>,
    items: &[(T, &str, &str)],
) -> Result<T> {
    if let Some(v) = given {
        let label = items
            .iter()
            .find(|(i, _, _)| *i == v)
            .map(|(_, l, _)| *l)
            .unwrap_or("");
        cliclack::log::step(format!("{question}  {label}"))?;
        return Ok(v);
    }
    if !is_terminal() {
        return Err(no_terminal(flag, question));
    }
    let mut s = cliclack::select(question);
    for (v, label, hint) in items {
        s = s.item(v.clone(), label, hint);
    }
    Ok(s.interact()?)
}

/// Any number of strings from a list, with the flag's comma-separated form.
pub fn multiselect(
    question: &str,
    flag: &str,
    given: Option<Vec<String>>,
    items: &[(String, &str)],
) -> Result<Vec<String>> {
    if let Some(v) = given {
        cliclack::log::step(format!(
            "{question}  {}",
            if v.is_empty() {
                "(none)".to_string()
            } else {
                v.join(", ")
            }
        ))?;
        return Ok(v);
    }
    if !is_terminal() {
        return Err(no_terminal(flag, question));
    }
    let mut s = cliclack::multiselect(question).required(false);
    for (v, hint) in items {
        s = s.item(v.clone(), v, hint);
    }
    Ok(s.interact()?)
}
