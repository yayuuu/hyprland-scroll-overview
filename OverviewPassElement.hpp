#pragma once
#include "globals.hpp"
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

class CScrollOverviewPassElement : public IPassElement {
  public:
    CScrollOverviewPassElement();
    ~CScrollOverviewPassElement() override = default;

    std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override;
    ePassElementType              type() override {
        return EK_CUSTOM;
    }
    bool                          needsLiveBlur(Render::CRenderContext& ctx) override;
    bool                          needsPrecomputeBlur(Render::CRenderContext& ctx) override;
    std::optional<CBox>           boundingBox(Render::CRenderContext& ctx) override;
    CRegion                       opaqueRegion(Render::CRenderContext& ctx) override;

    const char*                   passName() override {
        return "CScrollOverviewPassElement";
    }
};

class COverviewShadowPassElement : public IPassElement {
  public:
    struct SData {
        PHLMONITORREF monitor;
        CBox          fullBox;
        CBox          cutoutBox;
        int           rounding      = 0;
        float         roundingPower = 2.F;
        int           range         = 0;
        int           renderPower   = 0;
        Config::CGradientValueData color;
        float         alpha        = 1.F;
        bool          ignoreWindow = true;
    };

    COverviewShadowPassElement(const SData& data_);
    ~COverviewShadowPassElement() override = default;

    std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override;
    ePassElementType              type() override {
        return EK_CUSTOM;
    }
    bool                          needsLiveBlur(Render::CRenderContext& ctx) override;
    bool                          needsPrecomputeBlur(Render::CRenderContext& ctx) override;
    std::optional<CBox>           boundingBox(Render::CRenderContext& ctx) override;
    CRegion                       opaqueRegion(Render::CRenderContext& ctx) override;

    const char*                   passName() override {
        return "COverviewShadowPassElement";
    }

  private:
    SData data;
};
