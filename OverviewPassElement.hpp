#pragma once
#include "globals.hpp"
#include <hyprland/src/render/pass/PassElement.hpp>

class CScrollOverviewPassElement : public IPassElement {
  public:
    CScrollOverviewPassElement();
    virtual ~CScrollOverviewPassElement() = default;

    virtual void                draw(const CRegion& damage);
    virtual bool                needsLiveBlur();
    virtual bool                needsPrecomputeBlur();
    virtual std::optional<CBox> boundingBox();
    virtual CRegion             opaqueRegion();

    virtual const char*         passName() {
        return "CScrollOverviewPassElement";
    }

    virtual ePassElementType type() {
        return EK_CUSTOM;
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
        CHyprColor    color;
        float         alpha        = 1.F;
        bool          ignoreWindow = true;
        bool          sharp        = false;
    };

    COverviewShadowPassElement(const SData& data_);
    virtual ~COverviewShadowPassElement() = default;

    virtual void                draw(const CRegion& damage);
    virtual bool                needsLiveBlur();
    virtual bool                needsPrecomputeBlur();
    virtual std::optional<CBox> boundingBox();
    virtual CRegion             opaqueRegion();

    virtual const char*         passName() {
        return "COverviewShadowPassElement";
    }

    virtual ePassElementType type() {
        return EK_CUSTOM;
    }

  private:
    SData data;
};
