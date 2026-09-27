/**
 * The CLI login handoff (docs/21#cli-login). What must hold: the loopback shape posts to
 * 127.0.0.1 and only 127.0.0.1, echoes the CLI's state, and needs a click; the code shape approves
 * through the host with the credential the host minted; nothing happens on render alone.
 */
import { act, cleanup, fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { FjarrCliLogin, isValidCallbackPort, readCliLoginRequest } from "../src/index.js";

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
});

const flush = async () => {
  await act(async () => {
    await Promise.resolve();
    await Promise.resolve();
  });
};

describe("FjarrCliLogin", () => {
  it("reads the CLI's request from the query string", () => {
    expect(readCliLoginRequest("?port=43121&state=abc")).toEqual({ port: 43121, state: "abc", code: undefined });
    expect(readCliLoginRequest("?code=ab2c-d3ef")).toEqual({ port: undefined, state: undefined, code: "AB2CD3EF" });
    expect(readCliLoginRequest("?port=not-a-port")).toEqual({ port: undefined, state: undefined, code: undefined });
    expect(readCliLoginRequest("")).toEqual({ port: undefined, state: undefined, code: undefined });
  });

  it("posts the credential to 127.0.0.1 with the CLI's state, only after a click", async () => {
    const fetchMock = vi.spyOn(globalThis, "fetch").mockResolvedValue(new Response("", { status: 200 }));
    const mint = vi.fn(async () => "cred-1");
    render(<FjarrCliLogin mintOperatorCredential={mint} request={{ port: 43121, state: "s-9" }} />);
    expect(mint).not.toHaveBeenCalled();
    expect(fetchMock).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole("button", { name: "Approve" }));
    await flush();
    expect(mint).toHaveBeenCalledTimes(1);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [url, init] = fetchMock.mock.calls[0]!;
    expect(url).toBe("http://127.0.0.1:43121/callback");
    expect(JSON.parse(String(init?.body))).toEqual({ credential: "cred-1", state: "s-9" });
    expect(screen.getByRole("status").textContent).toMatch(/close this tab/);
  });

  it("never takes a callback host: a port outside the unprivileged range is refused before any request", async () => {
    const fetchMock = vi.spyOn(globalThis, "fetch");
    const mint = vi.fn(async () => "cred-1");
    render(<FjarrCliLogin mintOperatorCredential={mint} request={{ port: 80, state: "s" }} />);
    expect((screen.getByRole("button", { name: "Approve" }) as HTMLButtonElement).disabled).toBe(true);
    expect(fetchMock).not.toHaveBeenCalled();
    expect(mint).not.toHaveBeenCalled();
    expect(isValidCallbackPort(80)).toBe(false);
    expect(isValidCallbackPort(70000)).toBe(false);
    expect(isValidCallbackPort(43121)).toBe(true);
  });

  it("approves a code through the host with the credential the host minted", async () => {
    const mint = vi.fn(async () => "cred-2");
    const approve = vi.fn(async () => undefined);
    render(<FjarrCliLogin mintOperatorCredential={mint} approveCode={approve} request={{ code: "AB2CD3EF" }} />);
    expect(screen.getByText("AB2CD3EF")).toBeTruthy();
    fireEvent.click(screen.getByRole("button", { name: "Approve" }));
    await flush();
    expect(approve).toHaveBeenCalledWith("AB2CD3EF", "cred-2");
    expect(screen.getByRole("status").textContent).toMatch(/terminal will pick it up/);
  });

  it("lets the human type the code when the CLI did not put it in the URL", async () => {
    const approve = vi.fn(async () => undefined);
    render(<FjarrCliLogin mintOperatorCredential={async () => "c"} approveCode={approve} request={{}} />);
    const button = screen.getByRole("button", { name: "Approve" }) as HTMLButtonElement;
    expect(button.disabled).toBe(true);
    fireEvent.change(screen.getByRole("textbox"), { target: { value: "ab2c-d3ef" } });
    expect(button.disabled).toBe(false);
    fireEvent.click(button);
    await flush();
    expect(approve).toHaveBeenCalledWith("AB2CD3EF", "c");
  });

  it("says so when the host failed, and never claims success it did not get", async () => {
    vi.spyOn(globalThis, "fetch").mockResolvedValue(new Response("", { status: 403 }));
    render(<FjarrCliLogin mintOperatorCredential={async () => "c"} request={{ port: 43121, state: "s" }} />);
    fireEvent.click(screen.getByRole("button", { name: "Approve" }));
    await flush();
    expect(screen.getByRole("alert").textContent).toMatch(/403/);
    expect(screen.queryByRole("status")).toBeNull();
  });
});
