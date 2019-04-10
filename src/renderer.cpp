﻿#include "renderer.hpp"

#include "util/color.hpp"
#include "util/math-helpers.hpp"

#include "scene.hpp"



Renderer::Renderer(Options const& options) :
	options(options),
	framebuffer(options.res)
{
	//Load the scene
	if        (options.scene_name=="cornell"     ) {
		scene = Scene::get_new_cornell     ();
	} else if (options.scene_name=="cornell-srgb") {
		scene = Scene::get_new_cornell_srgb();
	} else if (options.scene_name=="d65sphere"   ) {
		scene = Scene::get_new_d65sphere   ();
	} else if (options.scene_name=="srgb"        ) {
		scene = Scene::get_new_srgb        ();
	} else {
		fprintf(stderr,"Unrecognized scene \"%s\"!  (Supported scenes: \"cornell\", \"cornell-srgb\", \"d65sphere\", \"srgb\")\n",options.scene_name.c_str());
		throw -3;
	}

	//Allocate space for threads
	#if 0
		_threads.resize(1);
	#else
		_threads.resize(std::thread::hardware_concurrency());
	#endif
}
Renderer::~Renderer() {
	//Cleanup scene
	delete scene;
}

void Renderer::_print_progress() const {
	auto pretty_print_time = [](double secs) -> void {
		double days  = std::floor(secs/86400.0);
		secs -= 86400.0 * days;
		double hours = std::floor(secs/ 3600.0);
		secs -=  3600.0 * hours;
		double mins  = std::floor(secs/ 3600.0);
		secs -=    60.0 * mins;

		if (days>0.0) printf("%d days + ",static_cast<int>(days));
		printf(
			"%02d:%02d:%06.3f",
			static_cast<int>(hours),
			static_cast<int>(mins ),
			secs
		);
	};

	std::chrono::steady_clock::time_point time_now = std::chrono::steady_clock::now();
	double time_since_start = static_cast<double>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(time_now-_time_start).count()
	) * 1.0e-9;

	double part = static_cast<double>(_num_tiles_start-_tiles.size()) / static_cast<double>(_num_tiles_start);
	if (part<1.0) {
		if (part>0.0) {
			double expected_time_total = time_since_start / part;
			double expected_time_remaining = expected_time_total - time_since_start;
			printf("\rRender %.2f%% (ETA ",part*100.0);
			pretty_print_time(expected_time_remaining);
			printf(")           ");
			fflush(stdout);
		} else {
			printf("\rRender started                               ");
		}
	} else {
		printf("\rRender completed in ");
		pretty_print_time(time_since_start);
		printf(" seconds             \n");
	}
}

CIEXYZ_32F Renderer::_render_sample(Math::RNG& rng, size_t i,size_t j) {
	//Render sample within pixel (`i`,`j`).

	//TODO: comment
	//TODO: ELS

	//i=256; j=430;

	ST framebuffer_st(
		(static_cast<float>(i)+rand_1f(rng)) / static_cast<float>(framebuffer.res[0]),
		(static_cast<float>(j)+rand_1f(rng)) / static_cast<float>(framebuffer.res[1])
	);
	glm::vec2 framebuffer_ndc = framebuffer_st*2.0f - glm::vec2(1.0f);

	Dir camera_ray_dir;
	{
		glm::vec4 point = scene->camera.matr_PV_inv * glm::vec4( framebuffer_ndc, 0.0f, 1.0f );
		point /= point.w;
		camera_ray_dir = glm::normalize( Pos(point) - scene->camera.pos );
	}

	//Path tracing with explicit light sampling.
	#define EXPLICIT_LIGHT_SAMPLING

	//	Hero wavelength sampling.
	//		First, the spectrum is divided into some number of regions.  Then, the hero wavelength
	//			is selected randomly from the first region.
	nm lambda_0 = LAMBDA_MIN + rand_1f(rng)*LAMBDA_STEP;
	//		Subsequent wavelengths are defined implicitly as multiples of `LAMBDA_STEP` above
	//			`lambda_0`.  The vector of these wavelengths are the wavelengths that the light
	//			transport is computed along.

	std::function<SpectralRadiance::HeroSample(Ray const&,bool,unsigned,PrimBase const*)> L = [&](
		Ray const& ray, bool last_was_delta, unsigned depth, PrimBase const* ignore
	) -> SpectralRadiance::HeroSample
	{
		SpectralRadiance::HeroSample radiance(0);

		HitRecord hitrec;
		if (scene->intersect( ray,&hitrec, ignore )) {
			//Emission
			#ifdef EXPLICIT_LIGHT_SAMPLING
			//Only add if could not have been sampled on previous)
			if (last_was_delta) {
			#endif
				SpectralRadiance::HeroSample emitted_radiance =
					hitrec.prim->material->sample_emission(hitrec.st,lambda_0)
				;

				radiance += emitted_radiance;
			#ifdef EXPLICIT_LIGHT_SAMPLING
			}
			#endif

			if (depth+1u<MAX_DEPTH) {
				Pos hit_pos = ray.at(hitrec.dist);

				SpectralRecipSR::HeroSample brdf = hitrec.prim->material->sample_brdf(hitrec.st,lambda_0);

				#ifdef EXPLICIT_LIGHT_SAMPLING
				//Direct lighting
				{
					Dir shad_ray_dir;
					PrimBase const* light;
					float shad_pdf;

					scene->get_rand_toward_light(rng,hit_pos,&shad_ray_dir,&light,&shad_pdf);
					float n_dot_l = glm::dot(shad_ray_dir,hitrec.normal);
					if (n_dot_l>0.0f) {
						Ray ray_shad = { hit_pos, shad_ray_dir };
						HitRecord hitrec_shad;
						scene->intersect(ray_shad,&hitrec_shad,hitrec.prim);
						if (hitrec_shad.prim == light) {
							SpectralRadiance::HeroSample emitted_radiance =
								light->material->sample_emission(hitrec_shad.st,lambda_0)
							;

							radiance += emitted_radiance * n_dot_l * brdf / shad_pdf;
							//if (glm::length(radiance)>2000.0) {
 							//	int j = 6;
							//}
						}
					}
				}
				#endif

				//Indirect lighting
				if (glm::dot(brdf,brdf)>0.0f) {
					float pdf_dir;
					Dir ray_next_dir = Math::rand_coshemi(rng,&pdf_dir);
					ray_next_dir = Math::get_rotated_to(ray_next_dir,hitrec.normal);

					Ray ray_next = { hit_pos, ray_next_dir };

					float n_dot_l = glm::dot(ray_next_dir,hitrec.normal);
					if (n_dot_l>0.0f) {
						radiance += L(ray_next,false,depth+1u,hitrec.prim) * n_dot_l * brdf / pdf_dir;
					}
				}
			}
		}

		return radiance;
	};

	Ray ray_camera = { scene->camera.pos, camera_ray_dir };
	SpectralRadiance::HeroSample pixel_rad_est_infdepth = L(ray_camera,true,0u,nullptr);

	//Value of Monte-Carlo estimator for the radiant flux incident on the pixel due to paths of any
	//	length.
	#ifdef FLAT_FIELD_CORRECTION
		SpectralRadiantFlux::HeroSample pixel_flux_est_infdepth = pixel_rad_est_infdepth;
	#else
		SpectralRadiantFlux::HeroSample pixel_flux_est_infdepth = pixel_rad_est_infdepth * glm::dot( camera_ray_dir, scene->camera.dir );
	#endif

	//Convert each wavelength sample to CIE XYZ and average.
	CIEXYZ_32F ciexyz_avg = Color::specradflux_to_ciexyz( pixel_flux_est_infdepth, lambda_0 );

	return ciexyz_avg;
}
void       Renderer::_render_pixel (Math::RNG& rng, size_t i,size_t j) {
	//Accumulate samples into CIE XYZ instead of a spectrum (probably `SpectralRadiantFlux`).  This
	//	way we avoid quantization artifacts and a large memory overhead per-pixel (in a more-
	//	sophisticated renderer, the pixel data would be stored for longer, e.g. to do nontrivial
	//	reconstruction filtering), for the (small) cost of having to do the conversion to CIE XYZ
	//	for each sample.

	CIEXYZ_32F avg(0,0,0);
	for (size_t k=0;k<options.spp;++k) {
		avg += _render_sample(rng, i,j);
	}
	avg /= static_cast<float>(options.spp);

	framebuffer(i,j) = sRGB_A_F32( Color::ciexyz_to_srgb(avg), 1.0f );
}
void Renderer::_render_threadwork() {
	//Add ourself to the count of rendering threads
	uint32_t thread_index = _num_rendering++;

	/*
	Random number generator for each thread.  Note that this must be per-thread data; making it
	threadsafe and shared would be too slow, and making it simply shared (which is, unfortunately,
	what many simplistic implementations do with e.g. `rand()`) risks producing bogus results due to
	race conditions.

	There are several ways to make thread local variables (such as e.g. C++ `thread_local`), but
	just making a local variable in the thread function is probably the clearest, if not the
	cleanest.
	*/
	Math::RNG rng;
	//Seed the RNG with the thread index (hashed to increase variance).
	rng.seed(static_cast<Math::RNG::result_type>( get_hashed(thread_index) ));

	//Main render thread loop
	while (_render_continue) {
		//Atomically pull the next tile of un-rendered pixels off the list of un-rendered tiles.  If
		//	there are none, terminate the loop.  Also print the progress (inside the mutex so that
		//	it's threadsafe) every 10ms.

		Framebuffer::Tile tile;

		std::chrono::steady_clock::time_point time_now = std::chrono::steady_clock::now();

		_tiles_mutex.lock();

		float time_since_last_print = static_cast<float>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(time_now-_time_last_print).count()
		) * 1.0e-9f;
		if (time_since_last_print>0.01f) {
			_print_progress();
			_time_last_print = time_now;
		}

		if (!_tiles.empty()) {
			tile = _tiles.back();
			_tiles.pop_back();

			_tiles_mutex.unlock();
		} else {
			_tiles_mutex.unlock();

			//	Minor optimization
			_render_continue = false;

			break;
		}

		//Render each pixel of the tile
		for (size_t j=tile.pos[1];j<tile.pos[1]+tile.res[1];++j) {
			for (size_t i=tile.pos[0];i<tile.pos[0]+tile.res[0];++i) {
				_render_pixel(rng, i,j);
			}
		}
	}

	//Remove ourself from the count of rendering threads
	assert(_num_rendering>0u);
	--_num_rendering;

	//If we're the last thread to finish, no threads can be touching the image anymore.  We are
	//	responsible for removing any un-rendered tiles (such as if the render was aborted) and
	//	saving the image to disk.
	if (_num_rendering==0u) {
		_tiles.clear();

		_print_progress();

		framebuffer.save(options.output_path);
	}
}
void Renderer::render_start() {
	//Create the list of tiles of un-rendered pixels
	assert(_tiles.empty());
	for (size_t j=0;j<options.res[1];j+=TILE_SIZE) {
		for (size_t i=0;i<options.res[0];i+=TILE_SIZE) {
			_tiles.push_back({
				{ i, j },
				{ std::min(options.res[0]-i,TILE_SIZE), std::min(options.res[1]-j,TILE_SIZE) }
			});
		}
	}
	//	TODO: describe
	std::reverse(_tiles.begin(),_tiles.end());

	//Starting information for timing
	_num_tiles_start = _tiles.size();
	_time_start      = std::chrono::steady_clock::now();
	_time_last_print = _time_start - std::chrono::seconds(1);

	//Create render threads (which also starts them working)
	_num_rendering = 0u;
	_render_continue = true;
	for (std::thread*& thread : _threads) {
		thread = new std::thread( &Renderer::_render_threadwork, this );
	}
}
void Renderer::render_wait () {
	//Wait for each render thread to terminate and clean up
	for (std::thread* thread : _threads) {
		thread->join();
		delete thread;
	}
	assert(_num_rendering==0u);
}
