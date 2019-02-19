//
// Created by Shiina Miyuki on 2019/2/18.
//

#include "mmlt.h"
#include "../../math/func.h"
#include "../../utils/stats.hpp"

using namespace Miyuki;

Miyuki::Float Miyuki::MLTSampler::nextFloat() {
    int index = getNextIndex();
    ensureReady(index);
    return X[index].value;
}

Miyuki::Point2f Miyuki::MLTSampler::nextFloat2D() {
    return {nextFloat(), nextFloat()};
}

void MLTSampler::ensureReady(int index) {
    if (index >= X.size())
        X.resize(index + 1u);
    auto &Xi = X[index];
    if (Xi.lastModificationIteration < lastLargeStepIteration) {
        Xi.value = randFloat();
        Xi.lastModificationIteration = lastLargeStepIteration;
    }
    Xi.backup();
    if (largeStep) {
        Xi.value = randFloat();
    } else {
        // Applying n small mutations of N(u1, sigma) is equivalent to sampling N(X, n*sigma^2)
        // TODO: why? Justify the math
        int64_t nSmall = currentIteration - Xi.lastModificationIteration;
        const Float Sqrt2 = std::sqrt(2);
        Float normalSample = Sqrt2 * erfInv(2 * randFloat() - 1);
        Float effSigma = sigma * std::sqrt((Float) nSmall);
        Xi.value += normalSample * effSigma;
        Xi.value -= std::floor(Xi.value);
        CHECK(0 <= Xi.value && Xi.value <= 1);
    }
    Xi.lastModificationIteration = currentIteration;
}

void MLTSampler::start() {

}

void MLTSampler::accept() {
    if (largeStep) {
        lastLargeStepIteration = currentIteration;
    }

}

void MLTSampler::reject() {
    for (PrimarySample &Xi :X) {
        if (Xi.lastModificationIteration == currentIteration) {
            Xi.restore();
        }
    }
    --currentIteration;
}

int MLTSampler::getNextIndex() {
    return streamIndex + streamCount * sampleIndex++;
}

void MLTSampler::startIteration() {
    currentIteration++;
    largeStep = randFloat() < largeStepProbability;
}

void MLTSampler::startStream(int index) {
    streamIndex = index;
    sampleIndex = 0;
}


Spectrum MultiplexedMLT::L(Scene &scene, MemoryArena &arena, MLTSampler &sampler, int depth, Point2i *raster) {
    sampler.startStream(MLTSampler::cameraStreamIndex);
    int s, t, nStrategies;
    if (depth == 0) {
        nStrategies = 1;
        s = 0;
        t = 2;
    } else {
        nStrategies = depth + 2;
        s = std::min((int) (sampler.nextFloat() * nStrategies), nStrategies - 1);
        t = nStrategies  - s;
    }
    auto cameraVertices = arena.alloc<Vertex>((size_t) t);
    auto lightVertices = arena.alloc<Vertex>((size_t) s);
    Point2f _pLens = sampler.nextFloat2D() * Point2f(scene.film.width(), scene.film.height());
    Point2i pLens;
    pLens.x() = clamp<int32_t>(_pLens.x(), 0, scene.film.width() - 1);
    pLens.y() = clamp<int32_t>(_pLens.y(), 0, scene.film.height() - 1);
    *raster = pLens;
    auto ctx = scene.getRenderContext(arena, pLens);
    ctx.sampler = &sampler;
    // Generate path with has a specific length
    if (generateCameraSubpath(scene, ctx, t, cameraVertices) != t)
        return {};
    sampler.startStream(MLTSampler::lightStreamIndex);
    if (generateLightSubpath(scene, ctx, s, lightVertices) != s)
        return {};
    sampler.startStream(MLTSampler::connectionStreamIndex);
    return removeNaNs(Spectrum(connectBDPT(scene, ctx, lightVertices, cameraVertices, s, t, raster) * nStrategies));

}

// Always continue until specific depth reached
Float MultiplexedMLT::continuationProbability(const Scene &scene, Float R, const Spectrum &beta, int depth) {
    return 1;
}

void MultiplexedMLT::render(Scene &scene) {
    delete[] samplers;
    nChains = (uint32_t) scene.option.mltNChains;
    nBootstrap = (uint32_t) scene.option.mltLuminanceSample;
    mltSeeds.resize(nChains);
    // spp = nChainMutations * nChains / (w * h)
    nChainMutations = static_cast<uint32_t>((double) scene.option.samplesPerPixel * scene.film.width() *
                                            scene.film.height()
                                            / (double) nChains);
    fmt::print("nChainMutations: {}\n", nChainMutations);
    auto maxDepth = scene.option.maxDepth;
    Float largeStepProb = scene.option.largeStepProb;
    std::random_device rd;
    {
        // compute bootstrap samples
        uint64_t nBootstrapSamples = nBootstrap * (maxDepth + 1);
        std::vector<Seed> seeds(nBootstrapSamples);
        std::vector<Seed> bootstrapSeeds(nBootstrapSamples);
        std::vector<Float> bootstrapWeights(nBootstrapSamples);

        {
            std::uniform_int_distribution<unsigned short> dist;
            for (int i = 0; i < seeds.size(); i++) {
                for (int j = 0; j < 3; j++) {
                    seeds[i][j] = dist(rd);
                    bootstrapSeeds[i][j] = seeds[i][j];
                }
            }
        }
        fmt::print("Generating bootstrap samples\n");
        ConcurrentMemoryArenaAllocator allocator;
        parallelFor(0u, nBootstrap, [&](uint32_t i) {
            auto arenaInfo = allocator.getAvailableArena();
            for (int depth = 0; depth <= maxDepth; depth++) {
                Point2i raster;
                int seedIndex = i * (maxDepth + 1) + depth;
                MLTSampler sampler(&bootstrapSeeds[seedIndex], largeStepProb, MLTSampler::nSampleStreams);
                bootstrapWeights[seedIndex] = luminance(L(scene, arenaInfo.arena, sampler, depth, &raster));
            }
            arenaInfo.arena.reset();
        });
        Distribution1D bootstrap(bootstrapWeights.data(), nBootstrapSamples);
        b = bootstrap.funcInt * (maxDepth + 1) / nBootstrapSamples;
        fmt::print("b: {}\n", b);
        // Selecting seeds according to distribution
        auto buffer = new uint8_t[sizeof(MLTSampler) * nChains];
        samplers = reinterpret_cast<MLTSampler *>(buffer);
        std::uniform_real_distribution<Float> dist;
        for (int i = 0; i < nChains; i++) {
            auto seedIndex = bootstrap.sampleInt(dist(rd));
            auto depth = seedIndex % (maxDepth + 1);
            for (int j = 0; j < 3; j++) {
                mltSeeds[i][j] = seeds[seedIndex][j];
            }
            new(&samplers[i])MLTSampler(&mltSeeds[i], largeStepProb, MLTSampler::nSampleStreams);
            samplers[i].depth = depth;
            auto arenaInfo = allocator.getAvailableArena();
            samplers[i].LCurrent = L(scene, arenaInfo.arena, samplers[i], samplers[i].depth, &samplers[i].pCurrent);
            arenaInfo.arena.reset();
        }
        // Let RAII auto clean up temporary vectors
    }
    int percentage = std::max<int>(nChainMutations / 100, 1);
    double elapsed = 0;
    ConcurrentMemoryArenaAllocator arenaAllocator;
    auto &film = scene.film;
    std::uniform_real_distribution<Float> dist;
    DECLARE_STATS(int32_t, acceptanceCounter);
    for (int iter = 0; iter < nChainMutations; iter++) {
        auto t = runtime([&]() {
            parallelFor(0u, nChains, [&](int i) {
                auto arenaInfo = arenaAllocator.getAvailableArena();
                auto &sampler = samplers[i];
                sampler.startIteration();
                Point2i pProposed;
                Spectrum LProposed = L(scene, arenaInfo.arena, sampler, sampler.depth, &pProposed);
                Float accept = std::min<Float>(1.0, luminance(LProposed) / luminance(sampler.LCurrent));
                CHECK(!std::isnan(accept));
                if (accept > 0) {
                    film.addSplat(pProposed, Spectrum(LProposed * accept / luminance(LProposed)));
                }
                film.addSplat(sampler.pCurrent,
                              Spectrum(sampler.LCurrent * (1 - accept) / luminance(sampler.LCurrent)));
                if (dist(rd) < accept) {
                    sampler.LCurrent = LProposed;
                    sampler.pCurrent = pProposed;
                    sampler.accept();
                    UPDATE_STATS(acceptanceCounter, 1);
                } else {
                    sampler.reject();
                }
                arenaInfo.arena.reset();
            });
        });
        elapsed += t;
        if (iter % percentage == 0) {
            fmt::print("Acceptance rate: {}\n", (double) acceptanceCounter / ((iter + 1) * nChains));
            fmt::print("Rendered {}%, elapsed {}s, remaining {}s\n",
                       (double) (iter + 1) / nChainMutations * 100,
                       elapsed,
                       (double) (elapsed * nChainMutations) / (iter + 1) - elapsed);
        }
    }
    film.scaleImageColor(b / scene.option.samplesPerPixel);
}