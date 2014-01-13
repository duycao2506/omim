#pragma once
#include "cell_id.hpp"
#include "data_factory.hpp"
#include "mwm_set.hpp"
#include "feature_covering.hpp"
#include "features_vector.hpp"
#include "scale_index.hpp"

#include "../coding/file_container.hpp"

#include "../../defines.hpp"

#include "../std/vector.hpp"
#include "../std/unordered_set.hpp"


class MwmValue : public MwmSet::MwmValueBase
{
public:
  FilesContainerR m_cont;
  IndexFactory m_factory;

  explicit MwmValue(string const & name);

  inline feature::DataHeader const & GetHeader() const { return m_factory.GetHeader(); }

  /// @return MWM file name without extension.
  string GetFileName() const;
};


class Index : public MwmSet
{
protected:
  /// @return mwm format version
  virtual int GetInfo(string const & name, MwmInfo & info) const;
  virtual MwmValue * CreateValue(string const & name) const;
  virtual void UpdateMwmInfo(MwmId id);

public:
  Index();
  ~Index();

  class MwmLock : public MwmSet::MwmLock
  {
    typedef MwmSet::MwmLock BaseT;
  public:
    MwmLock(Index const & index, MwmId mwmId)
      : BaseT(const_cast<Index &>(index), mwmId) {}

    inline MwmValue * GetValue() const
    {
      return static_cast<MwmValue *>(BaseT::GetValue());
    }

    /// @return MWM file name without extension.
    /// If value is 0, an empty string returned.
    string GetFileName() const;
  };

  bool DeleteMap(string const & fileName);
  bool UpdateMap(string const & fileName, m2::RectD & rect);

private:

  template <typename F>
  class ReadFeatureFunctor
  {
    FeaturesVector const & m_V;
    F & m_F;
    mutable unordered_set<uint32_t> m_offsets;
    MwmId m_mwmID;

  public:
    ReadFeatureFunctor(FeaturesVector const & v, F & f, MwmId mwmID)
      : m_V(v), m_F(f), m_mwmID(mwmID)
    {
    }

    void operator() (uint32_t offset) const
    {
      if (m_offsets.insert(offset).second)
      {
        FeatureType feature;

        m_V.Get(offset, feature);
        feature.SetID(FeatureID(m_mwmID, offset));

        m_F(feature);
      }
    }
  };

  /// old style mwm reading
  template <typename F>
  class ReadMWMFunctor
  {
  public:
    ReadMWMFunctor(F & f)
      : m_f(f)
    {
    }

    void operator() (MwmLock const & lock, covering::CoveringGetter & cov, uint32_t scale) const
    {
      MwmValue * pValue = lock.GetValue();
      if (pValue)
      {
        feature::DataHeader const & header = pValue->GetHeader();

        // Prepare needed covering.
        int const lastScale = header.GetLastScale();

        // In case of WorldCoasts we should pass correct scale in ForEachInIntervalAndScale.
        if (scale > lastScale) scale = lastScale;

        // Use last coding scale for covering (see index_builder.cpp).
        covering::IntervalsT const & interval = cov.Get(lastScale);

        // prepare features reading
        FeaturesVector fv(pValue->m_cont, header);
        ScaleIndex<ModelReaderPtr> index(pValue->m_cont.GetReader(INDEX_FILE_TAG),
                                         pValue->m_factory);

        // iterate through intervals
        ReadFeatureFunctor<F> f1(fv, m_f, lock.GetID());
        for (size_t i = 0; i < interval.size(); ++i)
          index.ForEachInIntervalAndScale(f1, interval[i].first, interval[i].second, scale);
      }
    }

  private:
    F & m_f;
  };

public:

  template <typename F>
  void ForEachInRect(F & f, m2::RectD const & rect, uint32_t scale) const
  {
    ReadMWMFunctor<F> implFunctor(f);
    ForEachInIntervals(implFunctor, covering::ViewportWithLowLevels, rect, scale);
  }

  template <typename F>
  void ForEachInRect_TileDrawing(F & f, m2::RectD const & rect, uint32_t scale) const
  {
    ReadMWMFunctor<F> implFunctor(f);
    ForEachInIntervals(implFunctor, covering::LowLevelsOnly, rect, scale);
  }

  template <typename F>
  void ForEachFeatureIDInRect(F & f, m2::RectD const & rect, uint32_t scale) const
  {
    ///TODO
    //ForEachInIntervals(ReadMWMFunctor(*this, f), rect, scale);
  }

  template <typename F>
  void ForEachInScale(F & f, uint32_t scale) const
  {
    ReadMWMFunctor<F> implFunctor(f);
    ForEachInIntervals(implFunctor, covering::FullCover, m2::RectD::GetInfiniteRect(), scale);
  }

  /// Guard for loading features from particular MWM by demand.
  class FeaturesLoaderGuard
  {
    MwmLock m_lock;
    FeaturesVector m_vector;

  public:
    FeaturesLoaderGuard(Index const & parent, MwmId id);

    inline MwmSet::MwmId GetID() const { return m_lock.GetID(); }
    inline string GetFileName() const { return m_lock.GetFileName(); }

    bool IsWorld() const;
    void GetFeature(uint32_t offset, FeatureType & ft);
  };

private:

  template <typename F>
  void ForEachInIntervals(F & f, covering::CoveringMode mode,
                          m2::RectD const & rect, uint32_t scale) const
  {
    vector<MwmInfo> mwm;
    GetMwmInfo(mwm);

    covering::CoveringGetter cov(rect, mode);

    size_t const count = mwm.size();
    MwmId worldID[2] = { count, count };

    for (MwmId id = 0; id < count; ++id)
    {
      if ((mwm[id].m_minScale <= scale && scale <= mwm[id].m_maxScale) &&
          rect.IsIntersect(mwm[id].m_limitRect))
      {
        switch (mwm[id].GetType())
        {
        case MwmInfo::COUNTRY:
          {
            MwmLock lock(*this, id);
            f(lock, cov, scale);
          }
          break;

        case MwmInfo::COASTS:
          worldID[0] = id;
          break;

        case MwmInfo::WORLD:
          worldID[1] = id;
          break;
        }
      }
    }

    if (worldID[0] < count)
    {
      MwmLock lock(*this, worldID[0]);
      f(lock, cov, scale);
    }

    if (worldID[1] < count)
    {
      MwmLock lock(*this, worldID[1]);
      f(lock, cov, scale);
    }
  }
};
