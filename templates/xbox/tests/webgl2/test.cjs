// WebGL2 バインディングの実行テスト (tests/webgl2/run.sh から起動)。
// player の WebGL バックエンドが使う経路を中心に、描画結果を readPixels で検証する。
"use strict";

const addon = require(process.env.NEXT2D_WEBGL2_ADDON);
const W = 64, H = 64;
const gl = addon.createContext(W, H);

let passed = 0, failed = 0;
const test = (name, fn) => {
    try {
        fn();
        const error = gl.getError();
        if (error !== gl.NO_ERROR) {
            throw new Error(`GL error 0x${error.toString(16)} left after test`);
        }
        ++passed;
        console.log(`ok   ${name}`);
    } catch (e) {
        ++failed;
        // 後続のテストへエラー状態を持ち越さない
        while (gl.getError() !== gl.NO_ERROR) { /* drain */ }
        console.log(`FAIL ${name}\n     ${e.message}`);
    }
};
const eq = (actual, expected, label) => {
    const a = Array.from(actual), b = Array.from(expected);
    if (a.length !== b.length || a.some((v, i) => Math.abs(v - b[i]) > 1)) {
        throw new Error(`${label}: expected [${b}] got [${a}]`);
    }
};
const assert = (cond, label) => { if (!cond) { throw new Error(label); } };

const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        throw new Error("compile: " + gl.getShaderInfoLog(shader));
    }
    return shader;
};
const link = (vs, fs) => {
    const program = gl.createProgram();
    const v = compile(gl.VERTEX_SHADER, vs), f = compile(gl.FRAGMENT_SHADER, fs);
    gl.attachShader(program, v);
    gl.attachShader(program, f);
    gl.linkProgram(program);
    gl.detachShader(program, v);
    gl.detachShader(program, f);
    gl.deleteShader(v);
    gl.deleteShader(f);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        throw new Error("link: " + gl.getProgramInfoLog(program));
    }
    return program;
};
const pixel = (x, y) => {
    const out = new Uint8Array(4);
    gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, out);
    return out;
};

// 画面全体を覆う 1 枚の三角形 (頂点バッファ不要)
const FULLSCREEN_VS = `#version 300 es
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;
const COLOR_FS = `#version 300 es
precision mediump float;
uniform vec4 u_color;
out vec4 o_color;
void main() { o_color = u_color; }`;
const TEXTURE_FS = `#version 300 es
precision mediump float;
uniform sampler2D u_texture;
in vec2 v_uv;
out vec4 o_color;
void main() { o_color = texture(u_texture, v_uv); }`;

const colorProgram = link(FULLSCREEN_VS, COLOR_FS);
const colorLocation = gl.getUniformLocation(colorProgram, "u_color");
const vao = gl.createVertexArray();
const fill = (r, g, b, a) => {
    gl.useProgram(colorProgram);
    gl.uniform4fv(colorLocation, new Float32Array([r, g, b, a]));
    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
};
const makeTarget = (w, h) => {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, w, h);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, "fbo incomplete");
    return { texture, fbo };
};

test("constants and context properties", () => {
    assert(gl.TRIANGLES === 4 && gl.RGBA8 === 0x8058 && gl.TEXTURE_2D_ARRAY === 0x8C1A, "GL constants");
    assert(gl.UNPACK_FLIP_Y_WEBGL === 0x9240, "WebGL constants");
    assert(gl.drawingBufferWidth === W && gl.drawingBufferHeight === H, "drawingBuffer size");
    assert(gl.isContextLost() === false, "isContextLost");
    assert(gl.getContextAttributes().stencil === true, "context attributes");
    assert(gl.getExtension("EXT_none") === null, "getExtension");
});

test("getParameter return types", () => {
    assert(typeof gl.getParameter(gl.VERSION) === "string", "VERSION");
    assert(gl.getParameter(gl.MAX_TEXTURE_SIZE) >= 2048, "MAX_TEXTURE_SIZE");
    assert(gl.getParameter(gl.MAX_FRAGMENT_UNIFORM_VECTORS) >= 224, "MAX_FRAGMENT_UNIFORM_VECTORS");
    gl.scissor(1, 2, 3, 4);
    const box = gl.getParameter(gl.SCISSOR_BOX);
    assert(box instanceof Int32Array, "SCISSOR_BOX type");
    eq(box, [1, 2, 3, 4], "SCISSOR_BOX");
    gl.scissor(0, 0, W, H);
    assert(gl.getParameter(gl.SCISSOR_TEST) === false, "SCISSOR_TEST");
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
    assert(gl.getParameter(gl.UNPACK_FLIP_Y_WEBGL) === true, "UNPACK_FLIP_Y_WEBGL");
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    assert(gl.getParameter(gl.STENCIL_BITS) === 8, "default framebuffer stencil bits");
});

test("draw to default framebuffer sets the present flag", () => {
    addon.swap();
    assert(addon.needsPresent() === false, "flag cleared by swap");
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, W, H);
    fill(1, 0, 0.5, 1);
    eq(pixel(10, 10), [255, 0, 128, 255], "fill color");
    assert(addon.needsPresent() === true, "flag set by draw");
});

test("draw to an offscreen framebuffer does not set the present flag", () => {
    addon.swap();
    const { fbo, texture } = makeTarget(8, 8);
    gl.viewport(0, 0, 8, 8);
    fill(0, 1, 0, 1);
    eq(pixel(3, 3), [0, 255, 0, 255], "offscreen fill");
    assert(addon.needsPresent() === false, "flag untouched");
    gl.deleteFramebuffer(fbo);
    gl.deleteTexture(texture);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
});

test("uniform setters: bound function, JS array, shader introspection", () => {
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, W, H);
    gl.useProgram(colorProgram);
    // player は uniform4fv.bind(gl, location) を保持して毎フレーム呼ぶ
    const setColor = gl.uniform4fv.bind(gl, colorLocation);
    setColor(new Float32Array([0, 0, 1, 1]));
    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    eq(pixel(1, 1), [0, 0, 255, 255], "bound uniform4fv");
    gl.uniform4fv(colorLocation, [1, 1, 0, 1]);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    eq(pixel(1, 1), [255, 255, 0, 255], "array uniform4fv");
    gl.uniform4f(colorLocation, 0, 1, 1, 1);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    eq(pixel(1, 1), [0, 255, 255, 255], "uniform4f");
    assert(gl.getProgramParameter(colorProgram, gl.ACTIVE_UNIFORMS) === 1, "ACTIVE_UNIFORMS");
    const info = gl.getActiveUniform(colorProgram, 0);
    assert(info.name === "u_color" && info.type === gl.FLOAT_VEC4 && info.size === 1, "getActiveUniform");
    assert(gl.getUniformLocation(colorProgram, "u_missing") === null, "missing uniform is null");
    gl.uniform4fv(null, new Float32Array(4));   // null location は黙って無視される
});

test("shader compile errors are reported, not thrown", () => {
    const shader = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(shader, "#version 300 es\nthis is not glsl");
    gl.compileShader(shader);
    assert(gl.getShaderParameter(shader, gl.COMPILE_STATUS) === false, "COMPILE_STATUS");
    assert(gl.getShaderInfoLog(shader).length > 0, "info log");
    gl.deleteShader(shader);
});

const textureProgram = link(FULLSCREEN_VS, TEXTURE_FS);
const drawTexture = (texture) => {
    gl.useProgram(textureProgram);
    gl.uniform1i(gl.getUniformLocation(textureProgram, "u_texture"), 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
};
// 上の行が赤、下の行が青の 2x2 テクスチャを作る (データは上の行から)
const TOP_RED_BOTTOM_BLUE = new Uint8Array([
    255, 0, 0, 255,  255, 0, 0, 255,
    0, 0, 255, 255,  0, 0, 255, 255,
]);
const upload2x2 = (flipY, source) => {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 2, 2);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, flipY);
    if (source) {
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, source);
    } else {
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 2, 2, gl.RGBA, gl.UNSIGNED_BYTE, TOP_RED_BOTTOM_BLUE);
    }
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    return texture;
};

test("texSubImage2D from typed array: UNPACK_FLIP_Y_WEBGL off / on", () => {
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, W, H);
    // 反転なし: データ先頭行が v=0 (画面下) に来る → 下が赤
    let texture = upload2x2(false);
    drawTexture(texture);
    eq(pixel(W / 2, 2), [255, 0, 0, 255], "no flip bottom");
    eq(pixel(W / 2, H - 3), [0, 0, 255, 255], "no flip top");
    gl.deleteTexture(texture);
    // 反転あり (player の既定): データ先頭行が画面上に来る → 上が赤
    texture = upload2x2(true);
    drawTexture(texture);
    eq(pixel(W / 2, H - 3), [255, 0, 0, 255], "flip top");
    eq(pixel(W / 2, 2), [0, 0, 255, 255], "flip bottom");
    gl.deleteTexture(texture);
});

test("texSubImage2D from an image source (canvas / ImageBitmap path)", () => {
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, W, H);
    const image = { width: 2, height: 2, data: TOP_RED_BOTTOM_BLUE };
    const texture = upload2x2(true, image);
    drawTexture(texture);
    eq(pixel(W / 2, H - 3), [255, 0, 0, 255], "image flip top");
    eq(pixel(W / 2, 2), [0, 0, 255, 255], "image flip bottom");
    gl.deleteTexture(texture);
});

test("UNPACK_PREMULTIPLY_ALPHA_WEBGL applies to image sources", () => {
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, W, H);
    const half = new Uint8Array(16);
    for (let i = 0; i < 4; ++i) { half.set([255, 255, 255, 128], i * 4); }
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
    const texture = upload2x2(false, { width: 2, height: 2, data: half });
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    gl.disable(gl.BLEND);
    drawTexture(texture);
    eq(pixel(5, 5), [128, 128, 128, 128], "premultiplied");
    gl.deleteTexture(texture);
});

test("undersized pixel data is rejected instead of read out of bounds", () => {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 64, 64);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 64, 64, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(16));
    assert(gl.getError() === gl.INVALID_OPERATION, "texSubImage2D short source");
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.readPixels(0, 0, 32, 32, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(16));
    assert(gl.getError() === gl.INVALID_OPERATION, "readPixels short destination");
    gl.deleteTexture(texture);
});

test("instanced drawing with per-instance attributes", () => {
    const program = link(`#version 300 es
layout(location = 0) in vec2 a_vertex;
layout(location = 1) in vec4 a_rect;
layout(location = 2) in vec4 a_color;
out vec4 v_color;
void main() {
    v_color = a_color;
    gl_Position = vec4(a_rect.xy + a_vertex * a_rect.zw, 0.0, 1.0);
}`, `#version 300 es
precision mediump float;
in vec4 v_color;
out vec4 o_color;
void main() { o_color = v_color; }`);
    const { fbo, texture } = makeTarget(16, 16);
    gl.viewport(0, 0, 16, 16);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const instancedVao = gl.createVertexArray();
    gl.bindVertexArray(instancedVao);
    const quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 1, 0, 0, 1, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    // 左半分 = 赤、右半分 = 緑 の 2 インスタンス。確保 → bufferSubData の順 (player と同じ)
    const instances = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, instances);
    const data = new Float32Array([
        -1, -1, 1, 2,  1, 0, 0, 1,
         0, -1, 1, 2,  0, 1, 0, 1,
    ]);
    gl.bufferData(gl.ARRAY_BUFFER, data.byteLength, gl.STREAM_DRAW);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, data.subarray(0, 16));
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 32, 0);
    gl.vertexAttribDivisor(1, 1);
    gl.enableVertexAttribArray(2);
    gl.vertexAttribPointer(2, 4, gl.FLOAT, false, 32, 16);
    gl.vertexAttribDivisor(2, 1);
    gl.useProgram(program);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, 2);
    eq(pixel(3, 8), [255, 0, 0, 255], "instance 0");
    eq(pixel(12, 8), [0, 255, 0, 255], "instance 1");
    gl.bindVertexArray(null);
    gl.deleteVertexArray(instancedVao);
    gl.deleteBuffer(quad);
    gl.deleteBuffer(instances);
    gl.deleteProgram(program);
    gl.deleteFramebuffer(fbo);
    gl.deleteTexture(texture);
});

test("stencil clip (player mask path): write mask then draw where equal", () => {
    const { fbo, texture } = makeTarget(16, 16);
    const stencil = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, stencil);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.STENCIL_INDEX8, 16, 16);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.STENCIL_ATTACHMENT, gl.RENDERBUFFER, stencil);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, "fbo incomplete");
    gl.viewport(0, 0, 16, 16);
    gl.clearColor(0, 0, 0, 0);
    gl.clearStencil(0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);
    // 1: 左半分だけステンシルへ 1 を書く (色は書かない)
    gl.enable(gl.STENCIL_TEST);
    gl.enable(gl.SCISSOR_TEST);
    gl.scissor(0, 0, 8, 16);
    gl.colorMask(false, false, false, false);
    gl.stencilMask(0xff);
    gl.stencilFunc(gl.ALWAYS, 1, 0xff);
    gl.stencilOp(gl.KEEP, gl.KEEP, gl.REPLACE);
    fill(1, 1, 1, 1);
    // 2: ステンシル == 1 の所だけ全面青を描く
    gl.disable(gl.SCISSOR_TEST);
    gl.colorMask(true, true, true, true);
    gl.stencilMask(0);
    gl.stencilFunc(gl.EQUAL, 1, 0xff);
    gl.stencilOp(gl.KEEP, gl.KEEP, gl.KEEP);
    fill(0, 0, 1, 1);
    gl.disable(gl.STENCIL_TEST);
    gl.stencilMask(0xff);
    eq(pixel(3, 8), [0, 0, 255, 255], "inside mask");
    eq(pixel(12, 8), [0, 0, 0, 0], "outside mask");
    gl.deleteRenderbuffer(stencil);
    gl.deleteFramebuffer(fbo);
    gl.deleteTexture(texture);
});

test("MSAA renderbuffer resolved with blitFramebuffer", () => {
    const samples = Math.min(4, gl.getParameter(gl.MAX_SAMPLES));
    const msaa = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, msaa);
    const color = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, color);
    gl.renderbufferStorageMultisample(gl.RENDERBUFFER, samples, gl.RGBA8, 16, 16);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, color);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, "msaa incomplete");
    gl.viewport(0, 0, 16, 16);
    fill(1, 0, 1, 1);
    const resolved = makeTarget(16, 16);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, msaa);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, resolved.fbo);
    gl.blitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    gl.bindFramebuffer(gl.FRAMEBUFFER, resolved.fbo);
    eq(pixel(8, 8), [255, 0, 255, 255], "resolved color");
    gl.deleteRenderbuffer(color);
    gl.deleteFramebuffer(msaa);
    gl.deleteFramebuffer(resolved.fbo);
    gl.deleteTexture(resolved.texture);
});

test("texture array: texStorage3D + framebufferTextureLayer + sampler2DArray", () => {
    const atlas = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, atlas);
    gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, gl.RGBA8, 4, 4, 2);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    const layerFbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, layerFbo);
    gl.viewport(0, 0, 4, 4);
    gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, atlas, 0, 0);
    fill(1, 0, 0, 1);
    gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, atlas, 0, 1);
    fill(0, 1, 0, 1);
    const program = link(FULLSCREEN_VS, `#version 300 es
precision mediump float;
precision mediump sampler2DArray;
uniform sampler2DArray u_atlas;
uniform float u_layer;
in vec2 v_uv;
out vec4 o_color;
void main() { o_color = texture(u_atlas, vec3(v_uv, u_layer)); }`);
    const out = makeTarget(4, 4);
    gl.useProgram(program);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, atlas);
    gl.uniform1i(gl.getUniformLocation(program, "u_atlas"), 0);
    gl.bindVertexArray(vao);
    gl.uniform1f(gl.getUniformLocation(program, "u_layer"), 1);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    eq(pixel(2, 2), [0, 255, 0, 255], "layer 1");
    gl.uniform1f(gl.getUniformLocation(program, "u_layer"), 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    eq(pixel(2, 2), [255, 0, 0, 255], "layer 0");
    gl.deleteProgram(program);
    gl.deleteFramebuffer(layerFbo);
    gl.deleteFramebuffer(out.fbo);
    gl.deleteTexture(out.texture);
    gl.deleteTexture(atlas);
});

test("pixel buffer binding rules: offsets need a bound buffer, views need none", () => {
    const pixels = new Uint8Array(4 * 4 * 4);
    // PIXEL_PACK_BUFFER 非束縛でオフセット指定 → 生ポインタ扱いにせず INVALID_OPERATION
    gl.readPixels(0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, 0);
    assert(gl.getError() === gl.INVALID_OPERATION, "expected INVALID_OPERATION");

    const pbo = gl.createBuffer();
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pbo);
    gl.bufferData(gl.PIXEL_PACK_BUFFER, 4 * 4 * 4, gl.STREAM_READ);
    // 束縛中に配列指定 → INVALID_OPERATION (配列は書き換わらない)
    pixels.fill(7);
    gl.readPixels(0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    assert(gl.getError() === gl.INVALID_OPERATION, "expected INVALID_OPERATION");
    assert(pixels.every((v) => v === 7), "array must stay untouched");
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    gl.deleteBuffer(pbo);

    // PIXEL_UNPACK_BUFFER 非束縛でオフセット指定のテクスチャ転送 → INVALID_OPERATION
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 4, 4, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    assert(gl.getError() === gl.NO_ERROR, "expected NO_ERROR");
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, 0);
    assert(gl.getError() === gl.INVALID_OPERATION, "expected INVALID_OPERATION");

    const upload = gl.createBuffer();
    gl.bindBuffer(gl.PIXEL_UNPACK_BUFFER, upload);
    gl.bufferData(gl.PIXEL_UNPACK_BUFFER, new Uint8Array(4 * 4 * 4).fill(200), gl.STATIC_DRAW);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    assert(gl.getError() === gl.INVALID_OPERATION, "expected INVALID_OPERATION");
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, 0);
    assert(gl.getError() === gl.NO_ERROR, "expected NO_ERROR");
    gl.bindBuffer(gl.PIXEL_UNPACK_BUFFER, null);
    gl.deleteBuffer(upload);
    gl.deleteTexture(tex);
});

test("async readback: PIXEL_PACK_BUFFER + fenceSync + getBufferSubData", () => {
    const { fbo, texture } = makeTarget(4, 4);
    gl.viewport(0, 0, 4, 4);
    fill(0.2, 0.4, 0.6, 1);
    const pbo = gl.createBuffer();
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pbo);
    gl.bufferData(gl.PIXEL_PACK_BUFFER, 4 * 4 * 4, gl.STREAM_READ);
    gl.readPixels(0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, 0);
    const sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl.flush();
    let status = gl.TIMEOUT_EXPIRED;
    for (let i = 0; i < 1000 && status === gl.TIMEOUT_EXPIRED; ++i) {
        status = gl.clientWaitSync(sync, gl.SYNC_FLUSH_COMMANDS_BIT, 1000000);
    }
    assert(status === gl.ALREADY_SIGNALED || status === gl.CONDITION_SATISFIED, "sync status " + status);
    gl.deleteSync(sync);
    const pixels = new Uint8Array(4 * 4 * 4);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, pixels);
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    eq(pixels.subarray(0, 4), [51, 102, 153, 255], "first pixel");
    eq(pixels.subarray(60, 64), [51, 102, 153, 255], "last pixel");
    gl.deleteBuffer(pbo);
    gl.deleteFramebuffer(fbo);
    gl.deleteTexture(texture);
});

test("wrapper objects are type checked and invalidated by delete", () => {
    const buffer = gl.createBuffer();
    gl.bindTexture(gl.TEXTURE_2D, buffer);       // 種別違いは null 扱い (束縛解除)
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.deleteBuffer(buffer);
    gl.deleteBuffer(buffer);                      // 二重削除は無視
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);       // 削除済みは null 扱い
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    gl.bindBuffer(gl.ARRAY_BUFFER, {});           // 無関係なオブジェクトも null 扱い
});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
