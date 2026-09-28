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

/// A line of text with a flag; `default` is offered on the terminal and taken by `--yes`.
pub fn input(
    question: &str,
    flag: &str,
    given: Option<String>,
    yes: bool,
    default: Option<&str>,
) -> Result<String> {
    if let Some(v) = given {
        cliclack::log::step(format!("{question}  {v}"))?;
        return Ok(v);
    }
    if yes {
        if let Some(d) = default {
            cliclack::log::step(format!("{question}  {d} (--yes)"))?;
            return Ok(d.to_string());
        }
        return Err(no_terminal(flag, question));
    }
    if !is_terminal() {
        return Err(no_terminal(flag, question));
    }
    let mut p = cliclack::input(question).required(true);
    if let Some(d) = default {
        p = p.default_input(d);
    }
    Ok(p.interact::<String>()?)
}

/// A secret with a flag (and its environment variable, for scripts that keep it off the command
/// line): never echoed, never shown as a settled step.
pub fn secret(question: &str, flag: &str, given: Option<String>) -> Result<String> {
    if let Some(v) = given {
        cliclack::log::step(format!("{question}  ••••••••"))?;
        return Ok(v);
    }
    if !is_terminal() {
        return Err(no_terminal(flag, question));
    }
    Ok(cliclack::password(question).mask('•').interact()?)
}

/// Any number of strings from a list, with the flag's comma-separated form; `--yes` takes
/// `initial`, which is also what the terminal shows pre-selected.
pub fn multiselect(
    question: &str,
    flag: &str,
    given: Option<Vec<String>>,
    yes: bool,
    initial: &[String],
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
    if yes {
        cliclack::log::step(format!(
            "{question}  {} (--yes)",
            if initial.is_empty() {
                "(none)".to_string()
            } else {
                initial.join(", ")
            }
        ))?;
        return Ok(initial.to_vec());
    }
    if !is_terminal() {
        return Err(no_terminal(flag, question));
    }
    let mut s = cliclack::multiselect(question)
        .required(false)
        .initial_values(initial.to_vec());
    for (v, hint) in items {
        s = s.item(v.clone(), v, hint);
    }
    Ok(s.interact()?)
}
