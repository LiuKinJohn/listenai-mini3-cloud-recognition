const MODEL = "@cf/cloudflare/clef-flash";
const MAX_IMAGE_BYTES = 350 * 1024;

const CLOUD_NAMES = {
  cirrus: "卷云",
  cirrocumulus: "卷积云",
  cirrostratus: "卷层云",
  altocumulus: "高积云",
  altostratus: "高层云",
  stratus: "层云",
  stratocumulus: "层积云",
  cumulus: "积云",
  cumulonimbus: "积雨云",
  uncertain: "无法判定"
};

function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" }
  });
}

function bytesToBase64(bytes) {
  let binary = "";
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return btoa(binary);
}

export function pickChoice(answer, fallback = "uncertain") {
  if (!answer) return fallback;
  if (typeof answer === "string") return answer;
  return answer.choice ?? answer.value ?? answer.answer ?? answer.label ?? fallback;
}

export function choiceConfidence(answer) {
  if (!answer || typeof answer !== "object") return 0;
  if (Number.isFinite(answer.confidence)) return answer.confidence;
  const choice = pickChoice(answer, "");
  const p = answer.probabilities?.[choice] ?? answer.probability;
  return Number.isFinite(p) ? p : 0;
}

function probability(answer) {
  if (Number.isFinite(answer)) return answer;
  if (!answer || typeof answer !== "object") return 0;
  return Number.isFinite(answer.noul) ? answer.noul :
    Number.isFinite(answer.probability) ? answer.probability : 0;
}

async function weatherAt(latitude, longitude) {
  if (!Number.isFinite(latitude) || !Number.isFinite(longitude)) return null;
  const params = new URLSearchParams({
    latitude: String(latitude),
    longitude: String(longitude),
    current: [
      "cloud_cover", "cloud_cover_low", "cloud_cover_mid", "cloud_cover_high",
      "relative_humidity_2m", "wind_speed_10m", "weather_code"
    ].join(","),
    timezone: "auto"
  });
  const response = await fetch(`https://api.open-meteo.com/v1/forecast?${params}`);
  if (!response.ok) return null;
  const result = await response.json();
  const current = result.current ?? {};
  return {
    cloud_cover: current.cloud_cover ?? null,
    cloud_low: current.cloud_cover_low ?? null,
    cloud_mid: current.cloud_cover_mid ?? null,
    cloud_high: current.cloud_cover_high ?? null,
    humidity: current.relative_humidity_2m ?? null,
    wind_kph: current.wind_speed_10m ?? null,
    weather_code: current.weather_code ?? null
  };
}

function evidenceFrom(answers, weather) {
  const evidence = [];
  const texture = pickChoice(answers.texture, "uncertain");
  const vertical = pickChoice(answers.vertical, "uncertain");
  const textureText = {
    fibrous: "纹理呈纤维状", layered: "云体呈连续层状", puffy: "可见团块与清晰边缘",
    towering: "有明显垂直发展", mixed: "画面包含混合云状", uncertain: "纹理特征不充分"
  }[texture];
  const verticalText = {
    low: "垂直发展较弱", medium: "有一定垂直发展", high: "垂直发展显著", uncertain: "高度特征不明确"
  }[vertical];
  evidence.push(textureText, verticalText);
  if (probability(answers.rain_bearing) >= 0.55) evidence.push("具有较强降水云特征");
  if (weather?.cloud_cover != null) evidence.push(`当地总云量约 ${Math.round(weather.cloud_cover)}%`);
  return evidence.filter(Boolean).slice(0, 4);
}

function locationFrom(cf = {}) {
  const latitude = Number(cf.latitude);
  const longitude = Number(cf.longitude);
  const label = [cf.city, cf.region, cf.country].filter(Boolean).join(" · ") || "未知位置";
  return {
    label,
    lat: Number.isFinite(latitude) ? latitude : null,
    lon: Number.isFinite(longitude) ? longitude : null,
    timezone: cf.timezone ?? "UTC"
  };
}

async function recognize(request, env) {
  if (!env.DEVICE_TOKEN) {
    return json({ error: "DEVICE_TOKEN secret is not configured" }, 503);
  }
  if (request.headers.get("x-device-token") !== env.DEVICE_TOKEN) {
    return json({ error: "unauthorized" }, 401);
  }
  const contentType = request.headers.get("content-type")?.split(";", 1)[0].trim();
  if (contentType !== "image/jpeg") return json({ error: "image/jpeg required" }, 415);

  const bytes = new Uint8Array(await request.arrayBuffer());
  if (!bytes.length || bytes.length > MAX_IMAGE_BYTES) {
    return json({ error: `image must be 1-${MAX_IMAGE_BYTES} bytes` }, 413);
  }

  const location = locationFrom(request.cf);
  const weather = await weatherAt(location.lat, location.lon);
  const state = {
    task: "Classify the dominant cloud genus visible in this sky photograph.",
    captured_at: new Date().toISOString(),
    location,
    weather,
    guidance: "Use visual morphology as primary evidence. Weather context is supporting evidence only. Select uncertain when sky visibility or evidence is insufficient."
  };
  const result = await env.AI.run(MODEL, {
    model: "clef-flash",
    state,
    images: [`data:image/jpeg;base64,${bytesToBase64(bytes)}`],
    questions: {
      sky_visible: {
        type: "noul",
        instructions: "Is enough sky and cloud visible to make a useful classification?"
      },
      cloud_type: {
        type: "choice",
        instructions: "Choose the dominant cloud genus or uncertain. Judge shape, texture, layering, edge definition, and vertical development.",
        criteria: {
          cirrus: "High, thin, fibrous or hair-like streaks",
          cirrocumulus: "Small high-level ripples or grain-like cloudlets",
          cirrostratus: "Thin high veil covering much of the sky, sometimes halo-forming",
          altocumulus: "Mid-level rounded patches or rolls, larger than cirrocumulus",
          altostratus: "Uniform mid-level gray or blue-gray sheet",
          stratus: "Low, uniform featureless layer resembling lifted fog",
          stratocumulus: "Low lumpy layer, rolls or broad connected cloud masses",
          cumulus: "Detached puffy heaps with flatter bases and modest vertical growth",
          cumulonimbus: "Deep towering convective cloud, often dark-based or anvil-shaped",
          uncertain: "Cloud is obscured, mixed, too small, or not reliably identifiable"
        }
      },
      texture: {
        type: "choice",
        instructions: "Which texture best describes the dominant visible cloud?",
        criteria: {
          fibrous: "Wispy or fibrous", layered: "Smooth and layered", puffy: "Rounded and puffy",
          towering: "Tall and vertically developed", mixed: "Several distinct textures", uncertain: "Not enough evidence"
        }
      },
      vertical: {
        type: "choice",
        instructions: "How strong is the visible vertical development?",
        criteria: { low: "Little", medium: "Moderate", high: "Strong or towering", uncertain: "Cannot tell" }
      },
      rain_bearing: {
        type: "noul",
        instructions: "Does the cloud show strong visual signs associated with precipitation or a storm?"
      }
    }
  });

  const answers = result.answers ?? {};
  let key = pickChoice(answers.cloud_type);
  if (probability(answers.sky_visible) < 0.45 || !CLOUD_NAMES[key]) key = "uncertain";
  const confidence = Math.max(0, Math.min(100, Math.round(choiceConfidence(answers.cloud_type) * 100)));
  return json({
    type: CLOUD_NAMES[key],
    confidence,
    captured_at: state.captured_at,
    location,
    evidence: evidenceFrom(answers, weather),
    weather,
    model: MODEL
  });
}

export default {
  async fetch(request, env) {
    try {
      const url = new URL(request.url);
      if (request.method === "GET" && url.pathname === "/health") {
        return json({ ok: true, service: "mini3-cloud-observer", model: MODEL });
      }
      if (request.method !== "POST" || url.pathname !== "/recognize") {
        return json({ error: "not found" }, 404);
      }
      return await recognize(request, env);
    } catch (error) {
      return json({ error: "recognition failed", detail: String(error?.message ?? error) }, 502);
    }
  }
};
