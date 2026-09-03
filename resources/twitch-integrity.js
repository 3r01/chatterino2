(() => {
    const setup = __SETUP__;
    const queuedMessages = [];
    let integrityToken = "";
    let integrityRefreshAt = 0;
    let integrityAuthorization = "";
    let integrityRefreshTimer = 0;
    let integrityRequest = null;
    let clientVersion = "";
    let clientVersionExpiresAt = 0;
    let clientVersionRequest = null;

    const getClientVersion = async (force = false) => {
        const now = Date.now();
        if (!force && clientVersion && now < clientVersionExpiresAt) {
            return clientVersion;
        }
        if (clientVersionRequest) {
            return clientVersionRequest;
        }

        const request = (async () => {
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 10000);
            try {
                const response = await window.fetch("https://www.twitch.tv/", {
                    credentials: "omit", cache: "no-store",
                    signal: controller.signal
                });
                if (!response.ok) {
                    throw new Error(
                        `Twitch version request failed (${response.status})`);
                }
                const html = await response.text();
                const match = html.match(
                    /__twilightBuildID\s*=\s*["']([^"']+)/);
                if (!match) {
                    throw new Error("Twitch returned no client version");
                }
                clientVersion = match[1];
                clientVersionExpiresAt = Date.now() + (15 * 60 * 1000);
                return clientVersion;
            } finally {
                clearTimeout(timeout);
            }
        })();
        clientVersionRequest = request;
        try {
            return await request;
        } finally {
            if (clientVersionRequest === request) {
                clientVersionRequest = null;
            }
        }
    };
    const send = value => {
        const message = JSON.stringify(value);
        if (window.chrome?.webview) {
            window.chrome.webview.postMessage(message);
        } else {
            queuedMessages.push(message);
        }
    };
    window.__chatterinoTakeMessage = () => queuedMessages.shift() ?? "";

    const scheduleIntegrityRefresh = (config, refreshAt) => {
        clearTimeout(integrityRefreshTimer);
        const delay = Math.max(
            0, Math.min(refreshAt - Date.now(), 0x7fffffff));
        integrityRefreshTimer = setTimeout(() => {
            void acquireIntegrity(config, true).catch(() => {});
        }, delay);
    };

    const acquireIntegrity = async (config, force) => {
        const now = Date.now();
        if (!force && integrityToken &&
            integrityAuthorization === config.authorization &&
            now < integrityRefreshAt) {
            return integrityToken;
        }
        if (integrityRequest) {
            try {
                await integrityRequest;
            } catch (_) {
                // The caller below will retry for its own authorization.
            }
            return acquireIntegrity(config, force);
        }

        const request = (async () => {
            const headers = {
                ...config.headers,
                "Client-Request-ID": crypto.randomUUID().replaceAll("-", "")
            };
            const response = await window.fetch(
                "https://gql.twitch.tv/integrity", {
                    method: "POST", mode: "cors", credentials: "omit",
                    headers
                });
            if (!response.ok) {
                throw new Error(
                    `Integrity request failed (${response.status})`);
            }
            const integrity = await response.json();
            const expiration = Number(integrity.expiration);
            if (!integrity.token || !Number.isFinite(expiration) ||
                expiration <= now) {
                throw new Error(
                    "Twitch returned an invalid integrity token");
            }

            integrityToken = integrity.token;
            integrityAuthorization = config.authorization;
            integrityRefreshAt = now + ((expiration - now) * 0.9);
            scheduleIntegrityRefresh(
                {authorization: config.authorization, headers},
                integrityRefreshAt);
            return integrityToken;
        })();
        integrityRequest = request;
        try {
            return await request;
        } finally {
            if (integrityRequest === request) {
                integrityRequest = null;
            }
        }
    };

    const execute = async (config, retry = true) => {
        try {
            const headers = {
                ...config.headers,
                "Client-Version": await getClientVersion()
            };
            const integrityConfig = {
                authorization: config.authorization,
                headers
            };
            const token = await acquireIntegrity(integrityConfig, false);
            const response = await window.fetch("https://gql.twitch.tv/gql", {
                method: "POST", mode: "cors", credentials: "omit",
                headers: {
                    ...headers,
                    "Accept": "application/json",
                    "Content-Type": "application/json",
                    "Client-Integrity": token
                },
                body: JSON.stringify(config.gqlBody)
            });
            const body = await response.text();
            if (retry && /failed integrity check/i.test(body)) {
                clientVersion = "";
                clientVersionExpiresAt = 0;
                await acquireIntegrity(integrityConfig, true);
                return execute(config, false);
            }
            send({
                type: "result", nonce: config.nonce,
                ok: response.ok, status: response.status, body
            });
        } catch (error) {
            send({
                type: "result", nonce: config.nonce,
                ok: false, error: String(error)
            });
        }
    };

    window.__chatterinoSendGif = config => void execute(config);
    const configureKasada = () => {
        window.KPSDK.configure([{
            protocol: "https:", method: "POST",
            domain: "gql.twitch.tv", path: "/integrity"
        }]);
    };
    if (typeof window.KPSDK?.configure === "function") {
        send({ type: "ready" });
    } else {
        document.addEventListener("kpsdk-load", configureKasada, {
            once: true
        });
        document.addEventListener("kpsdk-ready", () => {
            send({ type: "ready" });
        }, { once: true });

        const script = document.createElement("script");
        script.addEventListener("error", () => send({
            type: "startup-error",
            error: "Unable to load Twitch integrity code"
        }), { once: true });
        script.src = setup.kasadaScriptURL;
        document.body.appendChild(script);
    }
})();
