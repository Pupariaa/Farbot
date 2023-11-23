#include "update_beatmaps.h"

size_t nb_beatmaps_downloaded = 0;

void* download_beatmaps(void* arg) {
	sQueue* q = arg;
	size_t n = 0;
	size_t tmp = 0;
	FILE* file = NULL;
	char* bmid = NULL;
	char buffer[1024] = { 0 };
	while (1) {
		n = q->size;
		if (n == 0) {
			// no bmid has been found yet by the workers
			sleep(1);
			continue;
		}
		bmid = squeue_pop(q);
		if (!strcmp(bmid, "done")) {
			// all workers have terminated
			break;
		}
		n--;
		tmp = n * 1.2;
		// compute ETA
		int hours_left = tmp / 3600;
		int minutes_left = (tmp / 60) - (hours_left * 60);
		sprintf(buffer, "%dh%02d", hours_left, minutes_left);
		// print ETA
		printf("%zu maps left to download (%s)\n", n, buffer);
		sprintf(buffer, "https://osu.ppy.sh/osu/%s", bmid);
		// make request
		fflush(stderr);
		curl_request("GET", buffer);
		sprintf(buffer, "beatmaps/%s.osu", bmid);
		// save response
		if ((file = fopen(buffer, "wb+"))) {
			if (fwrite(curl_response.data, curl_response.size, 1, file))
				nb_beatmaps_downloaded++;
			else
				fprintf(stderr, "download_beatmaps: fwrite failed to write to %s.osu\n", bmid);
			fclose(file);
		}
		else
			fprintf(stderr, "download_beatmaps: fopen failed to create %s.osu\n", bmid);
		sleep(1);
	}
	return NULL;
}

void* _update_beatmaps(void* arg) {
	struct ThreadData* data = (struct ThreadData*)arg;
	FILE* bmset_file = NULL;
	char* bmset_filename = NULL;
	char* content = NULL;
	char bm_filename[128] = { 0 };
	json_t* tmp = NULL;
	const char* bmid = NULL;
	size_t index = 0;
	json_t* bm = NULL;
	for (size_t i = data->start; i < data->end; i++) {
		// get json file
		bmset_file = data->bmsets->files[i];
		bmset_filename = data->bmsets->filenames[i];
		// read json file
		fseek(bmset_file, 0, SEEK_END);
		size_t content_size = ftell(bmset_file);
		fseek(bmset_file, 0, SEEK_SET);
		content = realloc(content, content_size + 1);
		if (!content) {
			fprintf(stderr, "_update_beatmaps: %s failed, realloc failed\n", bmset_filename);
			fclose(bmset_file);
			continue;
		}
		fread(content, 1, content_size, bmset_file);
		fclose(bmset_file);
		content[content_size] = '\0';
		// parse content
		json_t* bmset = json_loads(content, 0, NULL);
		if (!bmset) {
			fprintf(stderr, "_update_beatmaps: %s failed, bad json\n", bmset_filename);
			continue;
		}
		if (json_is_null(bmset)) {
			fprintf(stderr, "_update_beatmaps: %s failed, null json\n", bmset_filename);
			json_decref(bmset);
			continue;
		}
		// seek for new beatmap ids
		json_array_foreach(bmset, index, bm) {
			if (!(tmp = json_object_get(bm, "beatmap_id"))) {
				fprintf(stderr, "_update_beatmaps: no beatmap_id in %zuth beatmap of %s\n", index, bmset_filename);
				continue;
			}
			bmid = json_string_value(tmp);
			if (!bmid) {
				fprintf(stderr, "_update_beatmaps: null beatmap_id in %zuth beatmap of %s\n", index, bmset_filename);
				continue;
			}
			sprintf(bm_filename, "beatmaps/%s.osu", bmid);
			if (access(bm_filename, F_OK) != -1)
				// bmid already downloaded
				continue;
			// give it to the download_beatmaps thread
			pthread_mutex_lock(&data->lock);
			squeue_push(data->q, bmid);
			pthread_mutex_unlock(&data->lock);
		}
		// release memory
		json_decref(bmset);
	}
	if (content)
		free(content);
	printf("%s - %s done (%zuth - %zuth)\n",
		data->bmsets->filenames[data->start],
		(data->end < data->bmsets->nb_files) ? data->bmsets->filenames[data->end] : "(null)",
		data->start, data->end);
	return NULL;
}

#define update_beatmaps_cleanup \
	{ \
		if (lock) \
			pthread_mutex_destroy(&lock); \
		if (dl_thread) \
			pthread_cancel(dl_thread); \
		close_files(bmsets); \
		if (ranges) \
			free(ranges); \
		if (threads) { \
			for (size_t j = 0; j < nb_workers; j++) { \
				if (threads[j]) pthread_cancel(threads[j]); \
			} \
			free(threads); \
		} \
		if (thread_datas) { \
			for (size_t j = 0; j < nb_workers; j++) { \
				if (thread_datas[j]) free(thread_datas[j]); \
			} \
			free(thread_datas); \
		} \
	}

int update_beatmaps(void) {
	// init lock
	pthread_mutex_t lock;
	if (pthread_mutex_init(&lock, NULL)) {
		fprintf(stderr, "update_beatmaps: pthread_mutex_init failed\n");
		return 1;
	}
	// init a iQueue
	sQueue* q = new_squeue(1024);
	if (!q) {
		fprintf(stderr, "update_beatmaps: new_iqueue failed\n");
		pthread_mutex_destroy(&lock);
		return 1;
	}
	// start download_beatmaps thread
	pthread_t dl_thread = 0;
	if (pthread_create(&dl_thread, NULL, download_beatmaps, q)) {
		fprintf(stderr, "update_beatmaps: pthread_create failed\n");
		pthread_mutex_destroy(&lock);
		// free_squeue(q);
		return 1;
	}
	// open all beatmapsets files
	printf("opening files...\n");
	FILES* bmsets = open_files("beatmapsets");
	if (!bmsets) {
		fprintf(stderr, "update_beatmaps: open_files failed\n");
		pthread_mutex_destroy(&lock);
		// free_squeue(q);
		pthread_cancel(dl_thread);
		return 1;
	}
	// init workers
	size_t nb_workers = min(bmsets->nb_files, 50);
	pthread_t* threads = calloc(nb_workers, sizeof(pthread_t));
	if (!threads) {
		fprintf(stderr, "update_beatmaps: open_files failed\n");
		pthread_mutex_destroy(&lock);
		// free_squeue(q);
		pthread_cancel(dl_thread);
		close_files(bmsets);
		return 1;
	}
	Range* ranges = divide(bmsets->nb_files, nb_workers);
	struct ThreadData** thread_datas = calloc(nb_workers, sizeof(struct ThreadData*));
	if (!threads) {
		fprintf(stderr, "update_beatmaps: calloc failed\n");
		update_beatmaps_cleanup;
		return 1;
	}
	for (size_t i = 0; i < nb_workers; i++) {
		thread_datas[i] = calloc(1, sizeof(struct ThreadData));
		if (!thread_datas[i]) {
			fprintf(stderr, "update_beatmaps: calloc failed\n");
			update_beatmaps_cleanup;
			return 1;
		}
		thread_datas[i]->start = ranges[i].start;
		thread_datas[i]->end = ranges[i].end;
		thread_datas[i]->bmsets = bmsets;
		thread_datas[i]->q = q;
		thread_datas[i]->lock = lock;
	}
	// start workers
	printf("updating beatmaps...\n");
	int st = time(NULL);
	for (size_t i = 0; i < nb_workers; i++) {
		if (pthread_create(&threads[i], NULL, _update_beatmaps, thread_datas[i])) {
			fprintf(stderr, "update_beatmaps: pthread_create failed\n");
			update_beatmaps_cleanup;
			return 1;
		}
	}
	// wait for the _update_beatmaps threads to find all new beatmap ids
	for (size_t i = 0; i < nb_workers; i++)
		pthread_join(threads[i], NULL);
	printf("took %ds to find new beatmaps\n", (int)(time(NULL) - st));
	// wait the download thread
	pthread_mutex_lock(&lock);
	squeue_push(q, "done");
	pthread_mutex_unlock(&lock);
	pthread_join(dl_thread, NULL);
	printf("took %ds to download new beatmaps\n", (int)(time(NULL) - st));
	// clean up
	update_beatmaps_cleanup;
	printf("%zu new beatmaps were downloaded\n", nb_beatmaps_downloaded);
	return 0;
}
