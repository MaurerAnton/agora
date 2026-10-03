# Image Generation

Open **Settings → Image Generation** to choose the model and default image size used by the image-generation tool.

## Model and credentials

The selected value identifies a configured `Provider:model`. Agora resolves the API key and base URL from that provider; this page does not maintain a separate image-generation key or endpoint. The picker favors image-capable models and can show all configured models when needed.

A model may be synchronized from a provider even if it is not enabled for ordinary chat. Whether it can actually generate images depends on the provider and model.

## Size

The default size is `1024x1024`. Available sizes and accepted options depend on the selected endpoint.

OpenRouter image models (for example `OpenRouter:openai/gpt-image-2.5-sunburst`, whose
output modality is image-only) are served over OpenRouter's dedicated
`POST {base}/images` API, not OpenAI's `/images/generations` and not `/chat/completions`.
When the selected image model's base URL points at `openrouter.ai`, Agora calls `/images`
with `{model, prompt, aspect_ratio, n: 1}`, converting the configured `WxH` size to the
nearest supported aspect ratio (`1:1, 3:2, 2:3, 4:3, 3:4, 16:9, 9:16, 21:9, auto`), and sends
the `HTTP-Referer`/`X-Title` headers OpenRouter requires. Use such models as the
**Image Generation** tool model and ask the chat model to call `generate_image`; selecting
an image-only model as the ordinary chat model cannot return text and will not render images.

## Data flow

The prompt and relevant request parameters are sent to the selected provider, and generated media is saved with the conversation/tool output on the device. See [Privacy & Security](privacy.md).
